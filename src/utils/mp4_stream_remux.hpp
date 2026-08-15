#ifndef MP4_STREAM_REMUX_HPP_
#define MP4_STREAM_REMUX_HPP_

// Public interface for the streaming MP4/ISOBMFF remuxer implemented in
// mp4_stream_remux.cpp. See that file for the full design rationale.
//
// Usage pattern (see StreamingRemuxSession.cpp for the real async version):
//   1) Fetch a small HEAD prefix (e.g. first 128KB) of the video-only and
//      audio-only source URLs (HTTP Range request).
//   2) TrackHead videoHead = parseHead(videoHeadBytes, "video");
//      TrackHead audioHead = parseHead(audioHeadBytes, "audio");
//      (parseHead throws std::runtime_error if the head buffer doesn't
//      contain a full moov + the mdat box header yet -- caller should
//      fetch a larger head and retry.)
//   3) RemuxPlan plan;
//      planStreamingRemux(videoHead, audioHead, &plan, &err);
//   4) preallocateAndWriteHead(outputPath, plan, &err);
//      -- output file now exists at its FINAL size with a valid
//      ftyp+moov+mdat-header already written.
//   5) Stream the audio body (Range: bytes=audioHead.mdatBodyOffsetInSource-)
//      and write it with writeBodyChunk(outputPath, plan.audioOutputOffset, ...).
//      Stream the video body (Range: bytes=videoHead.mdatBodyOffsetInSource-)
//      incrementally as it downloads, writing each chunk with
//      writeBodyChunk(outputPath, plan.videoOutputOffset + bytesSoFar, ...).
//
// C++03/GNU++98 only -- matches bbtube's QNX/gcc 4.6.3 toolchain. No
// `using` aliases, lambdas, non-static in-class member initializers, or
// std::initializer_list usage in this header or its .cpp.

#include <string>
#include <vector>
#include <stdint.h>

typedef std::vector<uint8_t> Mp4RemuxBytes;

// One entry from a parsed 'sidx' (segment index) box: byte range and
// duration of one fragment (a moof+mdat pair) within the source resource,
// relative to the position right after the sidx box itself.
struct SidxEntry {
    uint64_t referencedSize;   // bytes of this fragment (moof+mdat)
    uint32_t subsegmentDuration;
    SidxEntry() : referencedSize(0), subsegmentDuration(0) {}
};

// Result of parsing a 'sidx' box: where fragments start (byte offset in
// the source resource, right after the sidx box) and the list of
// fragments that follow it.
struct SidxInfo {
    size_t firstFragmentOffset; // absolute byte offset in the source resource
    std::vector<SidxEntry> entries;
    bool found;
    SidxInfo() : firstFragmentOffset(0), found(false) {}
};

// One decoded sample (from a 'trun' box) with its absolute byte offset
// and size within the source resource's mdat payload area.
struct FragSample {
    uint64_t offsetInSource; // absolute byte offset in the source resource
    uint32_t size;
    uint32_t duration;       // in the track's timescale
    FragSample() : offsetInSource(0), size(0), duration(0) {}
};

// Metadata for one track (video-only or audio-only source),
// parsed from just the HEAD bytes of that source -- no mdat payload needed.
struct TrackHead {
    std::string label;
    bool isVideo;
    uint32_t timescale;
    uint64_t duration;

    Mp4RemuxBytes tkhdBox, mdhdBox, hdlrBox, mediaHeaderBox;
    Mp4RemuxBytes stsdBox, sttsBox, cttsBox, stscBox, stszBox, stssBox;
    bool  stcoIs64;
    Mp4RemuxBytes stcoBox;

    size_t   mdatBodyOffsetInSource; // byte offset in the ORIGINAL source resource
    uint64_t mdatDeclaredSize;       // total payload bytes, from the mdat box header

    Mp4RemuxBytes ftypBox;

    // Fragmented-mp4 (DASH adaptiveFormats) support. When isFragmented is
    // true, sttsBox/stszBox/stcoBox above are NOT populated by parseHead;
    // instead sidx is set and the caller must fetch each fragment's moof
    // header (see FragSample/parseMoofSamples) and then call
    // buildProgressiveTablesFromFragments() to populate the sample tables
    // before planStreamingRemux() is used.
    bool isFragmented;
    SidxInfo sidx;
    std::vector<FragSample> fragSamples; // filled in across all fragments, in order

    TrackHead() : isVideo(false), timescale(0), duration(0), stcoIs64(false),
                  mdatBodyOffsetInSource(0), mdatDeclaredSize(0), isFragmented(false) {}
};

// Parses ftyp/moov/mdat-header out of `headBytes` (a prefix of the source
// file/resource). Throws std::runtime_error if headBytes doesn't extend far
// enough to contain a complete moov and the mdat box header -- caller
// should fetch more bytes and retry.
TrackHead parseHead(const Mp4RemuxBytes &headBytes, const std::string &label);

// The byte-exact layout of the OUTPUT file, computable from two TrackHeads
// alone (zero mdat payload bytes needed).
struct RemuxPlan {
    Mp4RemuxBytes headBytes;     // ftyp+moov+mdat-header, write at output offset 0
    size_t   videoOutputOffset;
    uint64_t videoBodySize;      // == video TrackHead's mdatDeclaredSize
    size_t   audioOutputOffset;
    uint64_t audioBodySize;
    uint64_t totalOutputSize;    // final size to pre-allocate the output file to

    RemuxPlan() : videoOutputOffset(0), videoBodySize(0), audioOutputOffset(0),
                  audioBodySize(0), totalOutputSize(0) {}
};

// NOTE: mutates videoHead.stcoBox / audioHead.stcoBox in place (patches
// chunk offsets to their final output positions) -- pass by non-const ref.
bool planStreamingRemux(TrackHead &videoHead, TrackHead &audioHead,
                         RemuxPlan *outPlan, std::string *errorOut);

// --- Fragmented-mp4 (DASH adaptiveFormats) support -------------------------
//
// YouTube/Invidious adaptiveFormats URLs serve DASH-style fragmented mp4:
// ftyp + moov (no real sample table -- just mvex/trex defaults) + sidx +
// a sequence of (moof + mdat) fragment pairs. parseHead() detects this
// (moov's stbl has an empty stco -- 0 entries) and sets isVideo/timescale/
// duration/ftypBox as before, plus isFragmented=true and (if a sidx box is
// present in headBytes) sidx.
//
// Caller workflow for a fragmented TrackHead:
//   1) parseHead() as usual. If result.isFragmented is true, continue below;
//      the usual sttsBox/stszBox/stcoBox are left empty.
//   2) If result.sidx.found is false, the head fetch wasn't large enough to
//      reach the sidx box -- refetch with a bigger head size and retry
//      parseHead(). (sidx normally sits right after moov, so this is rare.)
//   3) For each SidxEntry in result.sidx.entries (in order), issue a small
//      Range request for that fragment's moof (NOT its mdat payload -- moof
//      is typically a few hundred bytes at the start of the fragment) and
//      call parseMoofSamples() on the bytes received, passing the
//      fragment's absolute start offset (running sum of sidx.entries[i].
//      referencedSize, starting at sidx.firstFragmentOffset) and the
//      track's timescale. Append the returned samples to
//      result.fragSamples in fragment order.
//   4) Once every fragment's samples have been collected, call
//      buildProgressiveTablesFromFragments(result) to synthesize sttsBox/
//      stszBox/stcoBox/stscBox (stcoIs64 as needed) from result.fragSamples,
//      and mdatBodyOffsetInSource/mdatDeclaredSize spanning the *first*
//      sample to the *last* sample's end. From here on the TrackHead behaves
//      exactly like a non-fragmented one for planStreamingRemux() -- except
//      the source is no longer contiguous, so body streaming must fetch each
//      fragment's mdat individually (see StreamingRemuxSession) rather than
//      a single "Range: bytes=X-" covering the whole tail.

// Parses a 'sidx' box located anywhere in headBytes[searchStart,end).
// fragmentBaseOffset is the absolute byte offset in the source resource of
// the sidx box's OWN start (needed because sidx's first_offset field, if
// nonzero, is relative to the byte right after the sidx box). Returns a
// SidxInfo with found=false if no sidx box is present in range.
SidxInfo parseSidx(const Mp4RemuxBytes &headBytes, size_t searchStart, size_t end,
                    size_t sidxBoxAbsoluteStart);

// Parses one fragment's 'moof' box (moofBytes = just that fragment's moof,
// NOT including its mdat) and returns the samples it describes, with
// offsetInSource computed as fragmentStartOffset + (this fragment's moof
// size) + (per-sample offset from trun/tfhd). fragmentStartOffset is this
// fragment's absolute byte offset in the source resource (i.e. where its
// moof begins). Throws std::runtime_error if moofBytes doesn't contain a
// complete moof/traf/trun.
std::vector<FragSample> parseMoofSamples(const Mp4RemuxBytes &moofBytes,
                                          size_t fragmentStartOffset,
                                          const std::string &label);

// Synthesizes sttsBox/stszBox/stscBox/stcoBox (and stcoIs64,
// mdatBodyOffsetInSource, mdatDeclaredSize) from track.fragSamples (must be
// non-empty and in playback order). Leaves everything else in `track`
// untouched. Throws std::runtime_error on empty fragSamples.
void buildProgressiveTablesFromFragments(TrackHead &track);

// Creates/truncates outputPath, resizes it to plan.totalOutputSize, and
// writes plan.headBytes at offset 0. After this call the file is
// structurally valid ISOBMFF (openable/probeable) even though the mdat
// payload is still all zero bytes.
bool preallocateAndWriteHead(const std::string &outputPath, const RemuxPlan &plan,
                              std::string *errorOut);

// Writes `len` bytes from `data` at `outputOffset` in outputPath (opens,
// seeks, writes, closes). Safe to call repeatedly with increasing offsets
// as chunks of a track's body arrive over the network.
bool writeBodyChunk(const std::string &outputPath, uint64_t outputOffset,
                     const uint8_t *data, size_t len, std::string *errorOut);

#endif /* MP4_STREAM_REMUX_HPP_ */
