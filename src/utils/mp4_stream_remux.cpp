// mp4_stream_remux.cpp
//
// STREAMING MP4/ISOBMFF remuxer -- see mp4_stream_remux.hpp for the public
// interface and usage pattern, and the design-rationale comment block
// there. Short version: YouTube's adaptiveFormats mp4s put the moov box
// (full sample table) at the FRONT of the file, so the entire output
// layout can be computed from a small HEAD fetch (typically <1% of the
// file), before any sample payload has been downloaded. That lets us
// pre-allocate the final output file and stream both tracks' payload
// directly into their final byte positions as the network delivers them --
// no "wait for both downloads to finish, then remux" step.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain): no
// `using` aliases, lambdas, non-static in-class member initializers, or
// std::initializer_list usage anywhere in this file.

#include "mp4_stream_remux.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Low level big-endian helpers
// ---------------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t hi = rd32(p);
    uint64_t lo = rd32(p + 4);
    return (hi << 32) | lo;
}
static void wr32(Mp4RemuxBytes &out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
static std::string fourccStr(const uint8_t *p) {
    return std::string(reinterpret_cast<const char *>(p), 4);
}
static void append(Mp4RemuxBytes &dst, const Mp4RemuxBytes &src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// ---------------------------------------------------------------------------
// Box location within a byte range
// ---------------------------------------------------------------------------
struct BoxLoc {
    size_t headerOffset;
    size_t payloadOffset;
    size_t payloadSize;
    size_t totalSize;
    bool found;
    BoxLoc() : headerOffset(0), payloadOffset(0), payloadSize(0), totalSize(0), found(false) {}
};

static BoxLoc findBox(const Mp4RemuxBytes &data, size_t start, size_t end, const std::string &type,
                       bool allowPayloadBeyondEnd = false) {
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t sz32 = rd32(&data[pos]);
        std::string t = fourccStr(&data[pos + 4]);
        uint64_t boxSize = sz32;
        size_t headerLen = 8;
        if (sz32 == 1) {
            if (pos + 16 > end) break;
            boxSize = rd64(&data[pos + 8]);
            headerLen = 16;
        } else if (sz32 == 0) {
            // "extends to EOF" -- total size unknown until fully
            // downloaded. YouTube's faststart transcoder output always
            // writes a definite mdat size, so this shouldn't occur in
            // practice; fail loudly rather than guess.
            break;
        }
        if (boxSize < headerLen) break; // genuinely malformed
        if (pos + boxSize > end) {
            // This box's declared payload extends beyond what we've
            // fetched so far -- EXPECTED for mdat in a head-only buffer.
            // Return it (header-only info) if it's the box being searched
            // for; otherwise we can't safely skip past it to find
            // siblings, so stop scanning.
            if (t == type && allowPayloadBeyondEnd) {
                BoxLoc loc;
                loc.headerOffset = pos;
                loc.payloadOffset = pos + headerLen;
                loc.payloadSize = size_t(boxSize) - headerLen;
                loc.totalSize = size_t(boxSize);
                loc.found = true;
                return loc;
            }
            break;
        }
        if (t == type) {
            BoxLoc loc;
            loc.headerOffset = pos;
            loc.payloadOffset = pos + headerLen;
            loc.payloadSize = size_t(boxSize) - headerLen;
            loc.totalSize = size_t(boxSize);
            loc.found = true;
            return loc;
        }
        pos += size_t(boxSize);
    }
    return BoxLoc();
}

static Mp4RemuxBytes sliceBox(const Mp4RemuxBytes &data, const BoxLoc &loc) {
    return Mp4RemuxBytes(data.begin() + loc.headerOffset, data.begin() + loc.headerOffset + loc.totalSize);
}

static BoxLoc findBoxOrThrow(const Mp4RemuxBytes &data, size_t start, size_t end,
                              const std::string &sourceLabel, const char *boxName,
                              bool allowPayloadBeyondEnd = false) {
    BoxLoc loc = findBox(data, start, end, boxName, allowPayloadBeyondEnd);
    if (!loc.found)
        throw std::runtime_error(sourceLabel + ": no " + std::string(boxName) +
                                  " box found in head buffer (fetch a larger head?)");
    return loc;
}

// ---------------------------------------------------------------------------
// parseHead (public)
// ---------------------------------------------------------------------------
TrackHead parseHead(const Mp4RemuxBytes &headBytes, const std::string &label) {
    TrackHead t;
    t.label = label;
    const Mp4RemuxBytes &d = headBytes;
    size_t bufEnd = d.size();

    BoxLoc ftyp = findBoxOrThrow(d, 0, bufEnd, label, "ftyp");
    t.ftypBox = sliceBox(d, ftyp);

    BoxLoc moov = findBoxOrThrow(d, 0, bufEnd, label, "moov");
    BoxLoc mdat = findBoxOrThrow(d, 0, bufEnd, label, "mdat", /*allowPayloadBeyondEnd=*/true);
    t.mdatBodyOffsetInSource = mdat.payloadOffset;
    t.mdatDeclaredSize = mdat.payloadSize;

    BoxLoc trak = findBoxOrThrow(d, moov.payloadOffset, moov.payloadOffset + moov.payloadSize, label, "trak");
    size_t trakStart = trak.payloadOffset, trakEnd = trak.payloadOffset + trak.payloadSize;

    BoxLoc tkhd = findBoxOrThrow(d, trakStart, trakEnd, label, "tkhd");
    t.tkhdBox = sliceBox(d, tkhd);

    BoxLoc mdia = findBoxOrThrow(d, trakStart, trakEnd, label, "mdia");
    size_t mdiaStart = mdia.payloadOffset, mdiaEnd = mdia.payloadOffset + mdia.payloadSize;

    BoxLoc mdhd = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "mdhd");
    t.mdhdBox = sliceBox(d, mdhd);
    {
        const uint8_t *p = &d[mdhd.payloadOffset];
        uint8_t version = p[0];
        if (version == 1) {
            t.timescale = rd32(&d[mdhd.payloadOffset + 20]);
            t.duration = rd64(&d[mdhd.payloadOffset + 24]);
        } else {
            t.timescale = rd32(&d[mdhd.payloadOffset + 12]);
            t.duration = rd32(&d[mdhd.payloadOffset + 16]);
        }
    }

    BoxLoc hdlr = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "hdlr");
    t.hdlrBox = sliceBox(d, hdlr);
    t.isVideo = (fourccStr(&d[hdlr.payloadOffset + 8]) == "vide");

    BoxLoc minf = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "minf");
    size_t minfStart = minf.payloadOffset, minfEnd = minf.payloadOffset + minf.payloadSize;

    BoxLoc mhd = t.isVideo ? findBoxOrThrow(d, minfStart, minfEnd, label, "vmhd")
                            : findBoxOrThrow(d, minfStart, minfEnd, label, "smhd");
    t.mediaHeaderBox = sliceBox(d, mhd);

    BoxLoc stbl = findBoxOrThrow(d, minfStart, minfEnd, label, "stbl");
    size_t stblStart = stbl.payloadOffset, stblEnd = stbl.payloadOffset + stbl.payloadSize;

    t.stsdBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsd"));
    t.sttsBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stts"));
    t.stscBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsc"));
    t.stszBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsz"));

    BoxLoc ctts = findBox(d, stblStart, stblEnd, "ctts");
    if (ctts.found) t.cttsBox = sliceBox(d, ctts);
    if (t.isVideo) {
        BoxLoc stss = findBox(d, stblStart, stblEnd, "stss");
        if (stss.found) t.stssBox = sliceBox(d, stss);
    }

    BoxLoc stco = findBox(d, stblStart, stblEnd, "stco");
    if (stco.found) {
        t.stcoIs64 = false;
        t.stcoBox = sliceBox(d, stco);
    } else {
        t.stcoIs64 = true;
        t.stcoBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "co64"));
    }
    return t;
}

// ---------------------------------------------------------------------------
// moov construction helpers (internal)
// ---------------------------------------------------------------------------
static void shiftChunkOffsets(Mp4RemuxBytes &box, bool is64, int64_t delta) {
    uint32_t entryCount = rd32(&box[12]);
    size_t entrySize = is64 ? 8 : 4;
    size_t entriesStart = 16;
    for (uint32_t i = 0; i < entryCount; i++) {
        size_t off = entriesStart + size_t(i) * entrySize;
        if (is64) {
            uint64_t v = rd64(&box[off]);
            v = uint64_t(int64_t(v) + delta);
            box[off+0]=uint8_t(v>>56); box[off+1]=uint8_t(v>>48); box[off+2]=uint8_t(v>>40); box[off+3]=uint8_t(v>>32);
            box[off+4]=uint8_t(v>>24); box[off+5]=uint8_t(v>>16); box[off+6]=uint8_t(v>>8);  box[off+7]=uint8_t(v);
        } else {
            uint32_t v = rd32(&box[off]);
            v = uint32_t(int64_t(v) + delta);
            box[off+0]=uint8_t(v>>24); box[off+1]=uint8_t(v>>16); box[off+2]=uint8_t(v>>8); box[off+3]=uint8_t(v);
        }
    }
}
static void patchTkhdTrackId(Mp4RemuxBytes &tkhd, uint32_t newId) {
    uint8_t version = tkhd[8];
    size_t idOff = 8 + ((version == 1) ? 20 : 12);
    tkhd[idOff+0]=uint8_t(newId>>24); tkhd[idOff+1]=uint8_t(newId>>16);
    tkhd[idOff+2]=uint8_t(newId>>8);  tkhd[idOff+3]=uint8_t(newId);
}
static Mp4RemuxBytes makeBox(const std::string &type, const Mp4RemuxBytes &payload) {
    Mp4RemuxBytes out;
    wr32(out, uint32_t(payload.size() + 8));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
static Mp4RemuxBytes buildTrak(TrackHead &src, uint32_t newTrackId) {
    patchTkhdTrackId(src.tkhdBox, newTrackId);
    Mp4RemuxBytes stbl;
    append(stbl, src.stsdBox);
    append(stbl, src.sttsBox);
    if (!src.cttsBox.empty()) append(stbl, src.cttsBox);
    if (!src.stssBox.empty()) append(stbl, src.stssBox);
    append(stbl, src.stscBox);
    append(stbl, src.stszBox);
    append(stbl, src.stcoBox);
    Mp4RemuxBytes stblBox = makeBox("stbl", stbl);

    Mp4RemuxBytes urlEntry;
    wr32(urlEntry, 12);
    urlEntry.push_back('u'); urlEntry.push_back('r'); urlEntry.push_back('l'); urlEntry.push_back(' ');
    wr32(urlEntry, 0x00000001);
    Mp4RemuxBytes drefPayload; wr32(drefPayload, 0); wr32(drefPayload, 1); append(drefPayload, urlEntry);
    Mp4RemuxBytes dinf = makeBox("dinf", makeBox("dref", drefPayload));

    Mp4RemuxBytes minfPayload;
    append(minfPayload, src.mediaHeaderBox);
    append(minfPayload, dinf);
    append(minfPayload, stblBox);
    Mp4RemuxBytes minfBox = makeBox("minf", minfPayload);

    Mp4RemuxBytes mdiaPayload;
    append(mdiaPayload, src.mdhdBox);
    append(mdiaPayload, src.hdlrBox);
    append(mdiaPayload, minfBox);
    Mp4RemuxBytes mdiaBox = makeBox("mdia", mdiaPayload);

    Mp4RemuxBytes trakPayload;
    append(trakPayload, src.tkhdBox);
    append(trakPayload, mdiaBox);
    return makeBox("trak", trakPayload);
}
static Mp4RemuxBytes buildMvhd(uint32_t timescale, uint64_t duration, uint32_t nextTrackId) {
    Mp4RemuxBytes p;
    p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(0);
    wr32(p, 0); wr32(p, 0);
    wr32(p, timescale);
    wr32(p, uint32_t(duration));
    wr32(p, 0x00010000);
    p.push_back(0x01); p.push_back(0x00); p.push_back(0); p.push_back(0);
    wr32(p, 0); wr32(p, 0);
    static const uint32_t matrix[9] = {0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000};
    for (int i = 0; i < 9; i++) wr32(p, matrix[i]);
    for (int i = 0; i < 6; i++) wr32(p, 0);
    wr32(p, nextTrackId);
    return makeBox("mvhd", p);
}
static Mp4RemuxBytes buildFtypMoov(TrackHead &v, TrackHead &a) {
    uint32_t movieTimescale = 1000;
    uint64_t vDurMs = uint64_t(double(v.duration) / v.timescale * 1000.0);
    uint64_t aDurMs = uint64_t(double(a.duration) / a.timescale * 1000.0);
    uint64_t movieDur = std::max(vDurMs, aDurMs);

    Mp4RemuxBytes mvhd = buildMvhd(movieTimescale, movieDur, 3);
    Mp4RemuxBytes trakV = buildTrak(v, 1);
    Mp4RemuxBytes trakA = buildTrak(a, 2);

    Mp4RemuxBytes moovPayload;
    append(moovPayload, mvhd);
    append(moovPayload, trakV);
    append(moovPayload, trakA);
    Mp4RemuxBytes moovBox = makeBox("moov", moovPayload);

    Mp4RemuxBytes out;
    append(out, v.ftypBox);
    append(out, moovBox);
    return out;
}

// ---------------------------------------------------------------------------
// planStreamingRemux (public)
// ---------------------------------------------------------------------------
bool planStreamingRemux(TrackHead &videoHead, TrackHead &audioHead,
                         RemuxPlan *outPlan, std::string *errorOut) {
    try {
        if (!videoHead.isVideo) throw std::runtime_error("videoHead is not a video track");
        if (audioHead.isVideo)  throw std::runtime_error("audioHead is not an audio track");

        Mp4RemuxBytes head = buildFtypMoov(videoHead, audioHead);
        size_t mdatHeaderSize = 8;
        size_t outMdatStart = head.size() + mdatHeaderSize;

        // Audio goes first (small, finishes fast -- gets the whole
        // duration's audio available almost immediately); video streams
        // in after it.
        size_t audioOutStart = outMdatStart;
        size_t videoOutStart = outMdatStart + size_t(audioHead.mdatDeclaredSize);

        int64_t audioDelta = int64_t(audioOutStart) - int64_t(audioHead.mdatBodyOffsetInSource);
        int64_t videoDelta = int64_t(videoOutStart) - int64_t(videoHead.mdatBodyOffsetInSource);
        shiftChunkOffsets(audioHead.stcoBox, audioHead.stcoIs64, audioDelta);
        shiftChunkOffsets(videoHead.stcoBox, videoHead.stcoIs64, videoDelta);

        Mp4RemuxBytes finalHead = buildFtypMoov(videoHead, audioHead);
        if (finalHead.size() != head.size())
            throw std::runtime_error("internal error: moov size changed between passes");

        uint64_t totalMdat = mdatHeaderSize + audioHead.mdatDeclaredSize + videoHead.mdatDeclaredSize;
        Mp4RemuxBytes mdatHeader;
        wr32(mdatHeader, uint32_t(totalMdat));
        mdatHeader.push_back('m'); mdatHeader.push_back('d'); mdatHeader.push_back('a'); mdatHeader.push_back('t');

        outPlan->headBytes = finalHead;
        outPlan->headBytes.insert(outPlan->headBytes.end(), mdatHeader.begin(), mdatHeader.end());
        outPlan->audioOutputOffset = audioOutStart;
        outPlan->audioBodySize = audioHead.mdatDeclaredSize;
        outPlan->videoOutputOffset = videoOutStart;
        outPlan->videoBodySize = videoHead.mdatDeclaredSize;
        outPlan->totalOutputSize = outMdatStart + audioHead.mdatDeclaredSize + videoHead.mdatDeclaredSize;
        return true;
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// I/O helpers (public). Deliberately plain fstream (not Qt) so this file
// has no Qt dependency and stays independently testable; StreamingRemuxSession
// wraps these with QNetworkReply-driven calls.
// ---------------------------------------------------------------------------
bool preallocateAndWriteHead(const std::string &outputPath, const RemuxPlan &plan, std::string *errorOut) {
    try {
        std::ofstream out(outputPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create output: " + outputPath);
        if (plan.totalOutputSize > 0) {
            out.seekp(std::streamoff(plan.totalOutputSize - 1));
            char zero = 0;
            out.write(&zero, 1);
        }
        out.seekp(0);
        out.write(reinterpret_cast<const char *>(&plan.headBytes[0]), std::streamsize(plan.headBytes.size()));
        return true;
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}

bool writeBodyChunk(const std::string &outputPath, uint64_t outputOffset,
                     const uint8_t *data, size_t len, std::string *errorOut) {
    try {
        std::fstream out(outputPath.c_str(), std::ios::binary | std::ios::in | std::ios::out);
        if (!out) throw std::runtime_error("cannot open output for writing: " + outputPath);
        out.seekp(std::streamoff(outputOffset));
        out.write(reinterpret_cast<const char *>(data), std::streamsize(len));
        return bool(out);
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}
