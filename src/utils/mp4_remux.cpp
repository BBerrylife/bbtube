// mp4_remux.cpp
//
// Standalone MP4/ISOBMFF stream-copy remuxer.
//
// Purpose: BB10's mmrenderer can only play a single media source (it cannot
// decode+sync two separate elementary streams the way ExoPlayer's
// MergingMediaSource or a browser's MSE does). YouTube's high-resolution
// (720p/1080p+) formats are only available as *separate* video-only and
// audio-only adaptive streams. This tool downloads-then-combines them into
// one standard MP4 container (like `ffmpeg -c copy` does), WITHOUT
// re-encoding -- it just copies the compressed sample bytes and rewrites the
// container's box tree (moov) to describe two tracks pointing into one mdat.
//
// Why not just call the ffmpeg binary?
//  - BB10 BAR-packaged apps run in a permission-sandboxed environment;
//    spawning arbitrary external processes is restricted/unavailable for
//    typical app permissions.
//  - Cross-compiling ffmpeg for QNX/ARMv7 is a nontrivial undertaking with
//    little existing tooling, and would bloat the .bar package.
//  - The actual operation needed is narrow (stream copy / box rewrite), so a
//    small purpose-built implementation is far more portable and light.
//
// IMPORTANT: this file targets C++03/GNU++98, matching the QNX Momentics
// toolchain used to build bbtube (qcc / gcc 4.6.3, -lang-c++, no -std=c++0x
// passed by the project). Do NOT use C++11-only syntax here: no `using`
// type aliases, no lambdas, no non-static in-class member initializers, no
// braced-init-list / std::initializer_list overloads, no `auto`.
//
// Assumptions (matched to YouTube adaptiveFormats mp4 output):
//  - Each input is a standard non-fragmented MP4 (ftyp/moov/mdat), single
//    track (video-only or audio-only), as returned by YouTube's adaptive
//    formats for the itags bbtube already targets (H.264 video / AAC audio).
//  - stco/co64 entries are absolute file byte offsets (per ISO/IEC 14496-12),
//    so shifting a whole source's mdat payload by a constant delta keeps all
//    chunk-to-sample references correct without touching stsc/stsz/stts/ctts.
//
// Build (test/dev, desktop, simulating BB10's C++03 restriction):
//   g++ -O2 -std=gnu++98 -DMP4_REMUX_STANDALONE_MAIN -o mp4_remux mp4_remux.cpp
// Usage:
//   ./mp4_remux video_only.mp4 audio_only.mp4 output.mp4
//
// To embed in bbtube: this file compiles into a static remuxToFile()
// function. Call it in-process from YoutubeClient.cpp after both adaptive
// streams have finished downloading to local temp files, e.g.:
//   std::string err;
//   bool ok = remuxToFile(videoTmpPath.toStdString(), audioTmpPath.toStdString(),
//                          outputPath.toStdString(), &err);

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <stdint.h>

typedef std::vector<uint8_t> Bytes;

// ---------------------------------------------------------------------------
// Low level big-endian helpers (all ISOBMFF integers are big-endian)
// ---------------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t hi = rd32(p);
    uint64_t lo = rd32(p + 4);
    return (hi << 32) | lo;
}
static void wr32(Bytes &out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
static std::string fourccStr(const uint8_t *p) {
    return std::string(reinterpret_cast<const char *>(p), 4);
}

// ---------------------------------------------------------------------------
// Whole-file read helper
// ---------------------------------------------------------------------------
static Bytes readFile(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    Bytes data(static_cast<size_t>(sz), 0);
    if (sz > 0 && !f.read(reinterpret_cast<char *>(&data[0]), sz))
        throw std::runtime_error("cannot read: " + path);
    return data;
}

// ---------------------------------------------------------------------------
// Generic box location within a byte range. Returns {payloadOffset, payloadSize}
// relative to `data`, for the first box of type `type` found directly inside
// [start, end). payloadOffset points AFTER the 8/16-byte box header.
// ---------------------------------------------------------------------------
struct BoxLoc {
    size_t headerOffset;  // offset of box start (size field)
    size_t payloadOffset;
    size_t payloadSize;
    size_t totalSize;     // header + payload
    bool found;

    BoxLoc() : headerOffset(0), payloadOffset(0), payloadSize(0), totalSize(0), found(false) {}
};

static BoxLoc findBox(const Bytes &data, size_t start, size_t end, const std::string &type) {
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
            boxSize = end - pos; // extends to end of parent (rare, not expected here)
        }
        if (boxSize < headerLen || pos + boxSize > end) {
            break; // malformed / truncated -- stop scanning
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

static Bytes sliceBox(const Bytes &data, const BoxLoc &loc) {
    return Bytes(data.begin() + loc.headerOffset, data.begin() + loc.headerOffset + loc.totalSize);
}

// Find a box or throw, with a friendly message naming the source file.
static BoxLoc findBoxOrThrow(const Bytes &data, size_t start, size_t end,
                              const std::string &sourcePath, const char *boxName) {
    BoxLoc loc = findBox(data, start, end, boxName);
    if (!loc.found)
        throw std::runtime_error(sourcePath + ": no " + std::string(boxName) + " box");
    return loc;
}

// ---------------------------------------------------------------------------
// Parsed representation of one single-track source file
// ---------------------------------------------------------------------------
struct TrackSource {
    std::string path;
    Bytes file;

    bool isVideo;
    uint32_t timescale;
    uint64_t duration; // in track timescale units
    uint32_t origTrackId;

    // Raw sub-boxes copied verbatim (only stco/co64 values get patched)
    Bytes tkhdBox;   // full box incl header (track_id patched later)
    Bytes mdhdBox;   // full box incl header
    Bytes hdlrBox;   // full box incl header
    Bytes mediaHeaderBox; // vmhd or smhd, full box incl header
    Bytes stsdBox;   // full box incl header
    Bytes sttsBox;
    Bytes cttsBox;   // optional, may be empty
    Bytes stscBox;
    Bytes stszBox;
    Bytes stssBox;   // optional (sync samples, video only)
    bool  stcoIs64;
    Bytes stcoBox;   // stco or co64, full box incl header

    // mdat payload (raw sample bytes) + absolute offset it lived at in `file`
    size_t mdatDataOffset;
    size_t mdatDataSize;

    TrackSource()
        : isVideo(false), timescale(0), duration(0), origTrackId(1),
          stcoIs64(false), mdatDataOffset(0), mdatDataSize(0) {}
};

static TrackSource parseSource(const std::string &path) {
    TrackSource t;
    t.path = path;
    t.file = readFile(path);
    const Bytes &d = t.file;
    size_t fileEnd = d.size();

    BoxLoc moov = findBoxOrThrow(d, 0, fileEnd, path, "moov");
    BoxLoc mdat = findBoxOrThrow(d, 0, fileEnd, path, "mdat");
    t.mdatDataOffset = mdat.payloadOffset;
    t.mdatDataSize = mdat.payloadSize;

    BoxLoc trak = findBoxOrThrow(d, moov.payloadOffset, moov.payloadOffset + moov.payloadSize, path, "trak");
    size_t trakStart = trak.payloadOffset, trakEnd = trak.payloadOffset + trak.payloadSize;

    BoxLoc tkhd = findBoxOrThrow(d, trakStart, trakEnd, path, "tkhd");
    t.tkhdBox = sliceBox(d, tkhd);
    // tkhd version 0: track_id at payload offset 12 (after version/flags[4] + ctime/mtime[4+4])
    // version 1: 64-bit ctime/mtime -> track_id at offset 20
    {
        const uint8_t *p = &d[tkhd.payloadOffset];
        uint8_t version = p[0];
        size_t idOff = (version == 1) ? 20 : 12;
        t.origTrackId = rd32(&d[tkhd.payloadOffset + idOff]);
    }

    BoxLoc mdia = findBoxOrThrow(d, trakStart, trakEnd, path, "mdia");
    size_t mdiaStart = mdia.payloadOffset, mdiaEnd = mdia.payloadOffset + mdia.payloadSize;

    BoxLoc mdhd = findBoxOrThrow(d, mdiaStart, mdiaEnd, path, "mdhd");
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

    BoxLoc hdlr = findBoxOrThrow(d, mdiaStart, mdiaEnd, path, "hdlr");
    t.hdlrBox = sliceBox(d, hdlr);
    {
        // handler_type at payload offset 8 (after version/flags[4] + pre_defined[4])
        std::string handlerType = fourccStr(&d[hdlr.payloadOffset + 8]);
        t.isVideo = (handlerType == "vide");
    }

    BoxLoc minf = findBoxOrThrow(d, mdiaStart, mdiaEnd, path, "minf");
    size_t minfStart = minf.payloadOffset, minfEnd = minf.payloadOffset + minf.payloadSize;

    BoxLoc mhd = t.isVideo ? findBoxOrThrow(d, minfStart, minfEnd, path, "vmhd")
                            : findBoxOrThrow(d, minfStart, minfEnd, path, "smhd");
    t.mediaHeaderBox = sliceBox(d, mhd);

    BoxLoc stbl = findBoxOrThrow(d, minfStart, minfEnd, path, "stbl");
    size_t stblStart = stbl.payloadOffset, stblEnd = stbl.payloadOffset + stbl.payloadSize;

    t.stsdBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, path, "stsd"));
    t.sttsBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, path, "stts"));
    t.stscBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, path, "stsc"));
    t.stszBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, path, "stsz"));

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
        BoxLoc co64 = findBoxOrThrow(d, stblStart, stblEnd, path, "co64");
        t.stcoIs64 = true;
        t.stcoBox = sliceBox(d, co64);
    }

    return t;
}

// Shift every chunk offset in a (copied) stco/co64 box by `delta` bytes.
// Operates in place on the box's own byte buffer (header included).
// Box layout: size(4) type(4) version/flags(4) entry_count(4) entries...
//             offset0  offset4 offset8         offset12         offset16
static void shiftChunkOffsets(Bytes &box, bool is64, int64_t delta) {
    uint32_t entryCount = rd32(&box[12]);
    size_t entrySize = is64 ? 8 : 4;
    size_t entriesStart = 16;
    for (uint32_t i = 0; i < entryCount; i++) {
        size_t off = entriesStart + size_t(i) * entrySize;
        if (is64) {
            uint64_t v = rd64(&box[off]);
            v = uint64_t(int64_t(v) + delta);
            box[off + 0] = uint8_t(v >> 56); box[off + 1] = uint8_t(v >> 48);
            box[off + 2] = uint8_t(v >> 40); box[off + 3] = uint8_t(v >> 32);
            box[off + 4] = uint8_t(v >> 24); box[off + 5] = uint8_t(v >> 16);
            box[off + 6] = uint8_t(v >> 8);  box[off + 7] = uint8_t(v);
        } else {
            uint32_t v = rd32(&box[off]);
            v = uint32_t(int64_t(v) + delta);
            box[off + 0] = uint8_t(v >> 24); box[off + 1] = uint8_t(v >> 16);
            box[off + 2] = uint8_t(v >> 8);  box[off + 3] = uint8_t(v);
        }
    }
}

// Patch the 32-bit track_id field inside a copied tkhd box buffer.
static void patchTkhdTrackId(Bytes &tkhd, uint32_t newId) {
    uint8_t version = tkhd[8]; // payload starts at offset 8 (after size+type)
    size_t idOff = 8 + ((version == 1) ? 20 : 12);
    tkhd[idOff + 0] = uint8_t(newId >> 24);
    tkhd[idOff + 1] = uint8_t(newId >> 16);
    tkhd[idOff + 2] = uint8_t(newId >> 8);
    tkhd[idOff + 3] = uint8_t(newId);
}

// Wrap `payload` bytes with a box header of type `type`.
static Bytes makeBox(const std::string &type, const Bytes &payload) {
    Bytes out;
    wr32(out, uint32_t(payload.size() + 8));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

static void append(Bytes &dst, const Bytes &src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// Build one trak box for a source, given its new track_id.
static Bytes buildTrak(TrackSource &src, uint32_t newTrackId) {
    patchTkhdTrackId(src.tkhdBox, newTrackId);

    Bytes stbl;
    append(stbl, src.stsdBox);
    append(stbl, src.sttsBox);
    if (!src.cttsBox.empty()) append(stbl, src.cttsBox);
    if (!src.stssBox.empty()) append(stbl, src.stssBox);
    append(stbl, src.stscBox);
    append(stbl, src.stszBox);
    append(stbl, src.stcoBox);
    Bytes stblBox = makeBox("stbl", stbl);

    // dinf: minimal "self-contained data" box (dref with one "url " entry, flag=1)
    Bytes urlEntry;
    wr32(urlEntry, 12);
    urlEntry.push_back('u'); urlEntry.push_back('r'); urlEntry.push_back('l'); urlEntry.push_back(' ');
    wr32(urlEntry, 0x00000001);
    Bytes drefPayload;
    wr32(drefPayload, 0);
    wr32(drefPayload, 1);
    append(drefPayload, urlEntry);
    Bytes dref = makeBox("dref", drefPayload);
    Bytes dinf = makeBox("dinf", dref);

    Bytes minfPayload;
    append(minfPayload, src.mediaHeaderBox);
    append(minfPayload, dinf);
    append(minfPayload, stblBox);
    Bytes minfBox = makeBox("minf", minfPayload);

    Bytes mdiaPayload;
    append(mdiaPayload, src.mdhdBox);
    append(mdiaPayload, src.hdlrBox);
    append(mdiaPayload, minfBox);
    Bytes mdiaBox = makeBox("mdia", mdiaPayload);

    Bytes trakPayload;
    append(trakPayload, src.tkhdBox);
    append(trakPayload, mdiaBox);
    return makeBox("trak", trakPayload);
}

static Bytes buildMvhd(uint32_t timescale, uint64_t duration, uint32_t nextTrackId) {
    Bytes p;
    p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(0); // version/flags
    wr32(p, 0); wr32(p, 0);           // creation/modification time
    wr32(p, timescale);
    wr32(p, uint32_t(duration));
    wr32(p, 0x00010000);              // rate 1.0
    p.push_back(0x01); p.push_back(0x00); p.push_back(0); p.push_back(0); // volume 1.0 + reserved16
    wr32(p, 0); wr32(p, 0);
    static const uint32_t matrix[9] = {0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000};
    for (int i = 0; i < 9; i++) wr32(p, matrix[i]);
    for (int i = 0; i < 6; i++) wr32(p, 0);
    wr32(p, nextTrackId);
    return makeBox("mvhd", p);
}

// Build ftyp+moov for the given (already-patched-or-not) sources.
// firstInputPath is used only to source the ftyp box verbatim.
static Bytes buildFtypMoov(TrackSource &v, TrackSource &a, const std::string &firstInputPath) {
    Bytes ftypFile = readFile(firstInputPath);
    BoxLoc f = findBoxOrThrow(ftypFile, 0, ftypFile.size(), firstInputPath, "ftyp");
    Bytes ftypBox = sliceBox(ftypFile, f);

    uint32_t movieTimescale = 1000; // ms, arbitrary common choice
    uint64_t vDurMs = uint64_t(double(v.duration) / v.timescale * 1000.0);
    uint64_t aDurMs = uint64_t(double(a.duration) / a.timescale * 1000.0);
    uint64_t movieDur = std::max(vDurMs, aDurMs);

    Bytes mvhd = buildMvhd(movieTimescale, movieDur, 3 /* next_track_id */);
    Bytes trakV = buildTrak(v, 1);
    Bytes trakA = buildTrak(a, 2);

    Bytes moovPayload;
    append(moovPayload, mvhd);
    append(moovPayload, trakV);
    append(moovPayload, trakA);
    Bytes moovBox = makeBox("moov", moovPayload);

    Bytes out;
    append(out, ftypBox);
    append(out, moovBox);
    return out;
}

// Core remux entry point (safe to call in-process; no external binaries).
// Returns true on success; on failure, *errorOut (if non-null) is set.
static bool remuxToFile(const std::string &videoOnlyPath, const std::string &audioOnlyPath,
                         const std::string &outputPath, std::string *errorOut) {
    try {
        TrackSource vsrc = parseSource(videoOnlyPath);
        TrackSource asrc = parseSource(audioOnlyPath);
        if (!vsrc.isVideo) throw std::runtime_error("first input is not a video track");
        if (asrc.isVideo) throw std::runtime_error("second input is not an audio track");

        // 1) Build head (ftyp+moov) once with the sources' CURRENT stco values,
        //    purely to measure its exact byte size (size does not change once
        //    we later patch the stco VALUES in place).
        Bytes head = buildFtypMoov(vsrc, asrc, videoOnlyPath);
        size_t mdatHeaderSize = 8; // assumes payload < 4GB (true for a single video)
        size_t outMdatDataStart = head.size() + mdatHeaderSize;

        // 2) Compute where each source's sample data will live in the new
        //    mdat, and patch the stco/co64 boxes accordingly.
        size_t videoDataOutStart = outMdatDataStart;
        size_t audioDataOutStart = outMdatDataStart + vsrc.mdatDataSize;

        int64_t videoDelta = int64_t(videoDataOutStart) - int64_t(vsrc.mdatDataOffset);
        int64_t audioDelta = int64_t(audioDataOutStart) - int64_t(asrc.mdatDataOffset);

        shiftChunkOffsets(vsrc.stcoBox, vsrc.stcoIs64, videoDelta);
        shiftChunkOffsets(asrc.stcoBox, asrc.stcoIs64, audioDelta);

        // 3) Rebuild head with the now-correctly-patched stco boxes.
        Bytes finalHead = buildFtypMoov(vsrc, asrc, videoOnlyPath);
        if (finalHead.size() != head.size())
            throw std::runtime_error("internal error: moov size changed between passes");

        // 4) Write output: head + mdat(header + video payload + audio payload)
        uint64_t mdatTotalSize = mdatHeaderSize + vsrc.mdatDataSize + asrc.mdatDataSize;
        Bytes mdatHeader;
        wr32(mdatHeader, uint32_t(mdatTotalSize));
        mdatHeader.push_back('m'); mdatHeader.push_back('d'); mdatHeader.push_back('a'); mdatHeader.push_back('t');

        std::ofstream out(outputPath.c_str(), std::ios::binary);
        if (!out) throw std::runtime_error("cannot open output: " + outputPath);
        out.write(reinterpret_cast<const char *>(&finalHead[0]), std::streamsize(finalHead.size()));
        out.write(reinterpret_cast<const char *>(&mdatHeader[0]), std::streamsize(mdatHeader.size()));
        out.write(reinterpret_cast<const char *>(&vsrc.file[vsrc.mdatDataOffset]), std::streamsize(vsrc.mdatDataSize));
        out.write(reinterpret_cast<const char *>(&asrc.file[asrc.mdatDataOffset]), std::streamsize(asrc.mdatDataSize));
        return true;
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}

#ifdef MP4_REMUX_STANDALONE_MAIN
// Standalone CLI entry point for desktop testing only. When embedding this
// file into bbtube, define nothing extra -- just call remuxToFile(...)
// directly from C++ (e.g. wrapped with QString<->std::string conversions).
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " video_only.mp4 audio_only.mp4 output.mp4\n";
        return 1;
    }
    std::string err;
    if (!remuxToFile(argv[1], argv[2], argv[3], &err)) {
        std::cerr << "remux failed: " << err << "\n";
        return 1;
    }
    std::cerr << "OK: wrote " << argv[3] << "\n";
    return 0;
}
#endif
