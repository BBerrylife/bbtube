#ifndef STREAMINGREMUXSESSION_HPP_
#define STREAMINGREMUXSESSION_HPP_

#include "src/utils/mp4_stream_remux.hpp"

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>

// Manages downloading a video-only + audio-only adaptive stream pair and
// remuxing them into a single local MP4 file WHILE they download, instead
// of waiting for both downloads to finish first. See mp4_stream_remux.hpp
// for the underlying box-parsing/layout logic.
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

private:
    void requestVideoHead();
    void requestAudioHead();
    void tryBeginPlan();
    void beginBodyDownloads();
    void failWith(const QString &message);
    void checkAllDone();

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
};

#endif /* STREAMINGREMUXSESSION_HPP_ */
