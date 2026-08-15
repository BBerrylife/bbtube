#ifndef STREAMINGREMUXSESSION_HPP_
#define STREAMINGREMUXSESSION_HPP_

#include "src/utils/mp4_stream_remux.hpp"

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QList>

// Manages downloading a video-only + audio-only adaptive stream pair and
// remuxing them into a single local MP4 file WHILE they download, instead
// of waiting for both downloads to finish first. See mp4_stream_remux.hpp
// for the underlying box-parsing/layout logic.
//
// Two source shapes are supported, auto-detected per track from parseHead():
//   - Plain progressive mp4 (contiguous mdat, real stco/stsz in moov): the
//     original fast path -- head fetch, then one Range-streamed body fetch.
//   - DASH-fragmented mp4 (YouTube/Invidious adaptiveFormats: ftyp+moov+
//     sidx+[moof+mdat]xN, empty stco/stsz in moov): after the head fetch,
//     a short "fragment discovery" pass fetches just each fragment's small
//     moof header (via sidx-derived byte ranges) to build a real
//     progressive sample table, THEN body streaming proceeds fragment by
//     fragment, copying each fragment's real mdat bytes to their final
//     per-sample output positions.
//
// Typical usage (see PlayerPage.cpp):
//   StreamingRemuxSession *session = new StreamingRemuxSession(
//       networkManager, videoUrl, audioUrl, outputPath, this);
//   connect(session, SIGNAL(headReady()), this, SLOT(onRemuxHeadReady()));
//   connect(session, SIGNAL(failed(QString)), this, SLOT(onRemuxFailed(QString)));
//   session->start();
//   // once headReady() fires, session->outputPath() is a valid, playable
//   // (if still-growing) local MP4 file.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class StreamingRemuxSession: public QObject
{
Q_OBJECT
public:
    StreamingRemuxSession(QNetworkAccessManager *networkManager, const QString &videoUrl,
            const QString &audioUrl, const QString &outputPath, QObject *parent = 0);
    virtual ~StreamingRemuxSession();

    void start();
    QString outputPath() const
    {
        return m_outputPath;
    }

    // Synchronously disconnects every signal/slot connection this session
    // owns (including its own internal reply->finished() connections) and
    // aborts all in-flight network replies, before returning. Call this
    // BEFORE deleteLater()-ing a session that's being replaced (e.g. the
    // person changed quality while discovery was still in progress) --
    // deleteLater() alone only schedules destruction for the next event
    // loop iteration, and QNetworkReply::abort() can synchronously emit
    // finished() on some Qt/BB10 builds; if that happens before this
    // session's own destructor runs, its finished-handler slots
    // (onVideoMoofFinished() etc.) would still fire and touch member state
    // concurrently with -- or after -- teardown has started, which is a
    // use-after-free landmine. Disconnecting internal signal/slot wiring
    // first guarantees none of that session's own slots can run again,
    // regardless of what abort() does synchronously vs. asynchronously.
    void cancel();

signals:
    // Output file has been pre-allocated to its final size with a valid
    // ftyp+moov+mdat header written -- safe to open/probe/start playing
    // from the beginning now (playback should stay behind
    // videoBytesWritten() though; see progress()).
    void headReady();

    // Fires as each video chunk is written to disk.
    void progress(qint64 videoBytesWritten, qint64 videoBytesTotal);

    void audioComplete();
    void finished(); // both tracks fully written -- output file is complete
    void failed(QString errorMessage);

private slots:
    void onVideoHeadFinished();
    void onAudioHeadFinished();
    void onAudioBodyFinished();
    void onVideoBodyReadyRead();
    void onVideoBodyFinished();

    // Fragment-discovery path (fragmented sources only).
    void onVideoMoofFinished();
    void onAudioMoofFinished();
    // Fragment-by-fragment body streaming path (fragmented sources only).
    void onVideoFragBodyFinished();
    void onAudioFragBodyFinished();

private:
    void requestVideoHead();
    void requestAudioHead();
    void tryBeginPlan();
    void beginBodyDownloads();
    void failWith(const QString &message);
    void checkAllDone();

    // Fragment-discovery helpers (fragmented sources only).
    void beginFragmentDiscovery(TrackHead &track, bool isVideoTrack);
    void dispatchMoofRequests(bool isVideoTrack); // fills the concurrency window
    void onTrackFragmentsReady(bool isVideoTrack);

    // Fragment-by-fragment body streaming helpers (fragmented sources only).
    void beginFragmentedBodyDownloads();
    void requestNextFragBody(bool isVideoTrack);

    QNetworkAccessManager *m_networkManager;
    QString m_videoUrl;
    QString m_audioUrl;
    QString m_outputPath;

    int m_videoHeadFetchSize;
    int m_audioHeadFetchSize;
    bool m_videoHeadParsed;
    bool m_audioHeadParsed;
    bool m_planStarted;
    bool m_failed;

    TrackHead m_videoHead;
    TrackHead m_audioHead;
    RemuxPlan m_plan;

    QNetworkReply *m_videoHeadReply;
    QNetworkReply *m_audioHeadReply;
    QNetworkReply *m_videoBodyReply;
    QNetworkReply *m_audioBodyReply;

    QFile *m_outputFile; // kept open for the duration of the video body stream
    qint64 m_videoBytesWritten;
    bool m_audioDone;
    bool m_videoDone;

    // --- fragmented-source state ---
    bool m_videoFragmentsReady;
    bool m_audioFragmentsReady;
    // Discovery cursor: m_video/audioFragDiscoveryIndex is the next fragment
    // index still needing a moof request DISPATCHED (may be dispatched
    // out of completion order since several run concurrently -- see
    // MOOF_DISCOVERY_CONCURRENCY); m_video/audioFragDiscoveryCompleted
    // counts how many have finished (regardless of dispatch order) so we
    // know when the whole track is done. m_video/audioFragDiscoveryOffset
    // is a per-fragment-index running offset table computed once up front
    // (from sidx) since offsets can no longer be derived by simply adding
    // the previous fragment's size to a single cursor once requests are
    // in flight out of order.
    size_t m_videoFragDiscoveryIndex;
    size_t m_audioFragDiscoveryIndex;
    size_t m_videoFragDiscoveryCompleted;
    size_t m_audioFragDiscoveryCompleted;
    std::vector<uint64_t> m_videoFragOffsets; // [i] = absolute source offset of fragment i's moof
    std::vector<uint64_t> m_audioFragOffsets;
    QList<QNetworkReply *> m_videoMoofReplies; // in-flight, order not significant
    QList<QNetworkReply *> m_audioMoofReplies;
    // Per-fragment sample storage, indexed by fragment number -- needed
    // because completions arrive out of dispatch order once several
    // requests run concurrently, so samples can't just be appended to
    // TrackHead::fragSamples as each reply finishes. Concatenated back
    // into fragSamples (in fragment order) once every fragment is in.
    std::vector<std::vector<FragSample> > m_videoFragSlots;
    std::vector<std::vector<FragSample> > m_audioFragSlots;

    // Body-streaming cursor for fragmented sources: which sample index
    // we're currently writing (drives both the per-fragment source Range
    // request and the per-sample output offset), plus a running total of
    // bytes already written for this track so each write's output offset
    // is O(1) to compute instead of re-summing every prior sample size.
    size_t m_videoFragBodyIndex;
    size_t m_audioFragBodyIndex;
    qint64 m_videoFragBodyOffsetSoFar;
    qint64 m_audioFragBodyOffsetSoFar;
    QNetworkReply *m_videoFragBodyReply;
    QNetworkReply *m_audioFragBodyReply;
};

#endif /* STREAMINGREMUXSESSION_HPP_ */
