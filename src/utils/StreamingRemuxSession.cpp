#include "src/utils/StreamingRemuxSession.hpp"

#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

#ifdef QT_DEBUG
#include <QDebug>
#endif

// How much of each source to fetch first, trying to cover ftyp+moov in one
// request. Real YouTube adaptiveFormats moov boxes are typically a few KB
// to a few dozen KB; 128KB covers this comfortably for the vast majority of
// videos. If parseHead() still can't find a complete moov+mdat-header in
// this many bytes (e.g. an unusually long video with a huge sample table),
// we double the fetch size and retry, up to MAX_HEAD_FETCH_BYTES.
static const int INITIAL_HEAD_FETCH_BYTES = 128 * 1024;
static const int MAX_HEAD_FETCH_BYTES = 4 * 1024 * 1024;

static Mp4RemuxBytes qByteArrayToBytes(const QByteArray &arr)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(arr.constData());
    return Mp4RemuxBytes(p, p + arr.size());
}

StreamingRemuxSession::StreamingRemuxSession(QNetworkAccessManager *networkManager,
        const QString &videoUrl, const QString &audioUrl, const QString &outputPath,
        QObject *parent) :
        QObject(parent), m_networkManager(networkManager), m_videoUrl(videoUrl), m_audioUrl(
                audioUrl), m_outputPath(outputPath), m_videoHeadFetchSize(
                INITIAL_HEAD_FETCH_BYTES), m_audioHeadFetchSize(INITIAL_HEAD_FETCH_BYTES), m_videoHeadParsed(
                false), m_audioHeadParsed(false), m_planStarted(false), m_failed(false), m_videoHeadReply(
                0), m_audioHeadReply(0), m_videoBodyReply(0), m_audioBodyReply(0), m_outputFile(0), m_videoBytesWritten(
                0), m_audioDone(false), m_videoDone(false)
{
}

StreamingRemuxSession::~StreamingRemuxSession()
{
    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); }
    if (m_outputFile) {
        m_outputFile->close();
        delete m_outputFile;
    }
}

void StreamingRemuxSession::start()
{
    requestVideoHead();
    requestAudioHead();
}

void StreamingRemuxSession::requestVideoHead()
{
    // See the comment on beginBodyDownloads() below for why fromEncoded()
    // is used here instead of QNetworkRequest(m_videoUrl) directly.
    QNetworkRequest req(QUrl::fromEncoded(m_videoUrl.toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(m_videoHeadFetchSize - 1));
    m_videoHeadReply = m_networkManager->get(req);
    QObject::connect(m_videoHeadReply, SIGNAL(finished()), this, SLOT(onVideoHeadFinished()));
}

void StreamingRemuxSession::requestAudioHead()
{
    QNetworkRequest req(QUrl::fromEncoded(m_audioUrl.toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(m_audioHeadFetchSize - 1));
    m_audioHeadReply = m_networkManager->get(req);
    QObject::connect(m_audioHeadReply, SIGNAL(finished()), this, SLOT(onAudioHeadFinished()));
}

void StreamingRemuxSession::onVideoHeadFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_videoHeadReply;
    m_videoHeadReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("video head fetch failed: " + msg);
        return;
    }

    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    try {
        m_videoHead = parseHead(buf, "video");
        m_videoHeadParsed = true;
    } catch (const std::exception &e) {
        if (m_videoHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
            failWith(QString("video head parse failed (giving up after %1 bytes): %2")
                    .arg(m_videoHeadFetchSize).arg(e.what()));
            return;
        }
#ifdef QT_DEBUG
        qDebug() << "[bbtube][remux] video head" << m_videoHeadFetchSize
                 << "bytes not enough (" << e.what() << ") - doubling and retrying";
#endif
        m_videoHeadFetchSize *= 2;
        requestVideoHead();
        return;
    }

    tryBeginPlan();
}

void StreamingRemuxSession::onAudioHeadFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioHeadReply;
    m_audioHeadReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("audio head fetch failed: " + msg);
        return;
    }

    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    try {
        m_audioHead = parseHead(buf, "audio");
        m_audioHeadParsed = true;
    } catch (const std::exception &e) {
        if (m_audioHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
            failWith(QString("audio head parse failed (giving up after %1 bytes): %2")
                    .arg(m_audioHeadFetchSize).arg(e.what()));
            return;
        }
#ifdef QT_DEBUG
        qDebug() << "[bbtube][remux] audio head" << m_audioHeadFetchSize
                 << "bytes not enough (" << e.what() << ") - doubling and retrying";
#endif
        m_audioHeadFetchSize *= 2;
        requestAudioHead();
        return;
    }

    tryBeginPlan();
}

void StreamingRemuxSession::tryBeginPlan()
{
    if (m_planStarted || !m_videoHeadParsed || !m_audioHeadParsed) {
        return;
    }
    m_planStarted = true;

    std::string err;
    if (!planStreamingRemux(m_videoHead, m_audioHead, &m_plan, &err)) {
        failWith("remux planning failed: " + QString::fromStdString(err));
        return;
    }
    if (!preallocateAndWriteHead(m_outputPath.toStdString(), m_plan, &err)) {
        failWith("could not allocate output file: " + QString::fromStdString(err));
        return;
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux] plan ready - total output"
             << qint64(m_plan.totalOutputSize) << "bytes, video body"
             << qint64(m_plan.videoBodySize) << "bytes, audio body"
             << qint64(m_plan.audioBodySize) << "bytes -" << m_outputPath;
#endif

    emit headReady();
    beginBodyDownloads();
}

void StreamingRemuxSession::beginBodyDownloads()
{
    // Keep one QFile open for the duration of the (potentially long,
    // chunk-by-chunk) video body stream, instead of reopening the file on
    // every readyRead() -- audio is written in one shot separately below
    // since it's small and typically arrives as 1-2 chunks anyway.
    m_outputFile = new QFile(m_outputPath);
    if (!m_outputFile->open(QIODevice::ReadWrite)) {
        failWith("could not reopen output file for streaming writes: " + m_outputPath);
        return;
    }

    // m_videoUrl/m_audioUrl come straight from YouTube's videoplayback
    // URLs (via Invidious or InnerTube directly) and are ALREADY
    // percent-encoded (e.g. "aitags=133%2C134..."). QNetworkRequest's
    // QString constructor implicitly converts through QUrl(QString),
    // which treats the input as a *human-readable* URL and re-encodes
    // it -- turning an existing "%2C" into "%252C" (the literal '%'
    // getting encoded a second time) and corrupting the query string.
    // QUrl::fromEncoded() instead takes the bytes as already-valid
    // percent-encoded form and leaves them alone. This bug was silent
    // until the Invidious remux path was in regular use, since
    // (informally observed) not every server/instance seemed to choke
    // on the double-encoded query the same way.
    QNetworkRequest audioReq(QUrl::fromEncoded(m_audioUrl.toUtf8()));
    audioReq.setRawHeader("Range",
            "bytes=" + QByteArray::number(qint64(m_audioHead.mdatBodyOffsetInSource)) + "-");
    m_audioBodyReply = m_networkManager->get(audioReq);
    QObject::connect(m_audioBodyReply, SIGNAL(finished()), this, SLOT(onAudioBodyFinished()));

    QNetworkRequest videoReq(QUrl::fromEncoded(m_videoUrl.toUtf8()));
    videoReq.setRawHeader("Range",
            "bytes=" + QByteArray::number(qint64(m_videoHead.mdatBodyOffsetInSource)) + "-");
    m_videoBodyReply = m_networkManager->get(videoReq);
    QObject::connect(m_videoBodyReply, SIGNAL(readyRead()), this, SLOT(onVideoBodyReadyRead()));
    QObject::connect(m_videoBodyReply, SIGNAL(finished()), this, SLOT(onVideoBodyFinished()));
}

void StreamingRemuxSession::onAudioBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioBodyReply;
    m_audioBodyReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("audio body download failed: " + msg);
        return;
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    if (quint64(body.size()) != m_plan.audioBodySize) {
        // Server may not have honored the Range request (some CDNs ignore
        // Range on certain error/redirect paths) -- this would silently
        // corrupt the remux, so fail loudly instead.
        failWith(QString("audio body size mismatch: expected %1, got %2 -- server may not "
                "have honored the Range request").arg(qint64(m_plan.audioBodySize)).arg(
                body.size()));
        return;
    }

    if (!m_outputFile->seek(qint64(m_plan.audioOutputOffset))
            || m_outputFile->write(body) != body.size()) {
        failWith("failed writing audio body to output file");
        return;
    }

    m_audioDone = true;
    emit audioComplete();
    checkAllDone();
}

void StreamingRemuxSession::onVideoBodyReadyRead()
{
    if (m_failed || !m_videoBodyReply) return;

    QByteArray chunk = m_videoBodyReply->readAll();
    if (chunk.isEmpty()) return;

    if (!m_outputFile->seek(qint64(m_plan.videoOutputOffset) + m_videoBytesWritten)
            || m_outputFile->write(chunk) != chunk.size()) {
        failWith("failed writing video chunk to output file");
        return;
    }

    m_videoBytesWritten += chunk.size();
    emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));
}

void StreamingRemuxSession::onVideoBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_videoBodyReply;
    m_videoBodyReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("video body download failed: " + msg);
        return;
    }
    // Drain anything left in the reply's buffer that readyRead() hadn't
    // been fired for yet before finished().
    QByteArray tail = reply->readAll();
    reply->deleteLater();
    if (!tail.isEmpty()) {
        if (!m_outputFile->seek(qint64(m_plan.videoOutputOffset) + m_videoBytesWritten)
                || m_outputFile->write(tail) != tail.size()) {
            failWith("failed writing final video chunk to output file");
            return;
        }
        m_videoBytesWritten += tail.size();
        emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));
    }

    if (quint64(m_videoBytesWritten) != m_plan.videoBodySize) {
        failWith(QString("video body size mismatch: expected %1, wrote %2 -- server may not "
                "have honored the Range request").arg(qint64(m_plan.videoBodySize)).arg(
                m_videoBytesWritten));
        return;
    }

    m_videoDone = true;
    checkAllDone();
}

void StreamingRemuxSession::checkAllDone()
{
    if (m_audioDone && m_videoDone) {
        if (m_outputFile) {
            m_outputFile->close();
        }
        emit finished();
    }
}

void StreamingRemuxSession::failWith(const QString &message)
{
    if (m_failed) return; // only report the first failure
    m_failed = true;

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux] FAILED:" << message;
#endif

    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); m_videoHeadReply = 0; }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); m_audioHeadReply = 0; }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); m_videoBodyReply = 0; }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); m_audioBodyReply = 0; }
    if (m_outputFile) { m_outputFile->close(); }

    emit failed(message);
}
