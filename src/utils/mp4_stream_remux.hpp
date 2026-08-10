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

// Metadata + sample tables for one track (video-only or audio-only source),
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

    TrackHead() : isVideo(false), timescale(0), duration(0), stcoIs64(false),
                  mdatBodyOffsetInSource(0), mdatDeclaredSize(0) {}
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
