#include "src/utils/StreamingRemuxSession.hpp"

#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

#ifdef QT_DEBUG
#include <QDebug>
#endif

// How much of each source to fetch first, trying to cover ftyp+moov+sidx in
// one request. Real YouTube adaptiveFormats moov boxes are typically a few
// KB to a few dozen KB, and sidx (when present, for fragmented sources)
// immediately follows moov and is usually small too (a dozen bytes per
// fragment entry) -- 128KB covers this comfortably for the vast majority
// of videos. If parseHead() still can't find a complete moov+mdat-header
// (or, for fragmented sources, moov+sidx) in this many bytes, we double
// the fetch size and retry, up to MAX_HEAD_FETCH_BYTES.
static const int INITIAL_HEAD_FETCH_BYTES = 128 * 1024;
static const int MAX_HEAD_FETCH_BYTES = 4 * 1024 * 1024;

// How many bytes to fetch per fragment when discovering a fragmented
// source's real sample table (moof headers only, not the mdat payload
// that follows each one). moof boxes for adaptiveFormats are typically a
// few hundred bytes to a couple KB even at high sample counts; 8KB gives
// generous headroom without pulling in meaningful mdat payload.
static const int MOOF_DISCOVERY_FETCH_BYTES = 8 * 1024;

// How many fragment moof headers to request concurrently during
// discovery, per track. Sequentially requesting them one at a time (the
// original implementation) meant total discovery latency scaled with
// fragment count * per-request round-trip time -- for a long video with
// hundreds of fragments against a slow/distant Invidious instance this
// could take long enough that the person would give up and switch quality
// mid-discovery, aborting the in-flight session. Running several in
// parallel divides that latency by roughly this factor. Kept modest (not
// dozens) to avoid overwhelming the Invidious instance or BB10's own
// concurrent-connection handling.
static const int MOOF_DISCOVERY_CONCURRENCY = 5;

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
                0), m_audioDone(false), m_videoDone(false), m_videoFragmentsReady(false), m_audioFragmentsReady(
                false), m_videoFragDiscoveryIndex(0), m_audioFragDiscoveryIndex(0), m_videoFragDiscoveryCompleted(
                0), m_audioFragDiscoveryCompleted(0), m_videoFragBodyIndex(
                0), m_audioFragBodyIndex(0), m_videoFragBodyOffsetSoFar(0), m_audioFragBodyOffsetSoFar(
                0), m_videoFragBodyReply(0), m_audioFragBodyReply(0)
{
}

StreamingRemuxSession::~StreamingRemuxSession()
{
    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); }
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    if (m_videoFragBodyReply) { m_videoFragBodyReply->abort(); m_videoFragBodyReply->deleteLater(); }
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); }
    if (m_outputFile) {
        m_outputFile->close();
        delete m_outputFile;
    }
}

void StreamingRemuxSession::cancel()
{
    // Disconnect every signal/slot connection involving this object
    // (both directions: signals this object emits, and any reply/timer
    // signal currently wired to one of this object's slots) before
    // touching any network reply. deleteLater() alone only schedules
    // destruction for the next event loop iteration -- but
    // QNetworkReply::abort() can synchronously emit finished() on some
    // Qt/BB10 builds, and if that happens before this session's own
    // destructor runs, its finished-handler slots (onVideoMoofFinished()
    // etc.) would still fire and touch member state concurrently with,
    // or after, teardown has started: a use-after-free landmine.
    // Disconnecting first guarantees none of this session's own slots can
    // run again, regardless of whether abort() completes synchronously.
    QObject::disconnect(this, 0, 0, 0);

    m_failed = true; // belt-and-suspenders, in case anything still checks it

    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); m_videoHeadReply = 0; }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); m_audioHeadReply = 0; }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); m_videoBodyReply = 0; }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); m_audioBodyReply = 0; }
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    m_videoMoofReplies.clear();
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    m_audioMoofReplies.clear();
    if (m_videoFragBodyReply) { m_videoFragBodyReply->abort(); m_videoFragBodyReply->deleteLater(); m_videoFragBodyReply = 0; }
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); m_audioFragBodyReply = 0; }
    if (m_outputFile) {
        m_outputFile->close();
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
        // Fragmented sources need a sidx box to proceed; if the initial
        // head fetch didn't reach far enough to include one, treat that
        // the same as "not enough head yet" and retry with a bigger
        // fetch, same as the moov-too-small case below.
        if (m_videoHead.isFragmented && !m_videoHead.sidx.found) {
            if (m_videoHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
                failWith(QString("video head parse failed (giving up after %1 bytes): "
                        "fragmented source but no sidx box found").arg(m_videoHeadFetchSize));
                return;
            }
            m_videoHeadFetchSize *= 2;
            requestVideoHead();
            return;
        }
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

    if (m_videoHead.isFragmented) {
        beginFragmentDiscovery(m_videoHead, /*isVideoTrack=*/true);
    } else {
        tryBeginPlan();
    }
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
        if (m_audioHead.isFragmented && !m_audioHead.sidx.found) {
            if (m_audioHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
                failWith(QString("audio head parse failed (giving up after %1 bytes): "
                        "fragmented source but no sidx box found").arg(m_audioHeadFetchSize));
                return;
            }
            m_audioHeadFetchSize *= 2;
            requestAudioHead();
            return;
        }
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

    if (m_audioHead.isFragmented) {
        beginFragmentDiscovery(m_audioHead, /*isVideoTrack=*/false);
    } else {
        tryBeginPlan();
    }
}

// ---------------------------------------------------------------------------
// Fragment discovery (fragmented sources only): walk this track's sidx
// entries in order, fetching each fragment's small moof header (not its
// mdat payload) and accumulating decoded samples into track.fragSamples.
// Once every fragment has been visited, build the track's progressive
// sample tables from the accumulated samples and fall through to the
// normal tryBeginPlan()/beginBodyDownloads() flow, which from this point
// on treats the track exactly like a non-fragmented one EXCEPT that body
// streaming must go through the fragment-by-fragment path (see
// beginFragmentedBodyDownloads()) since the source's mdat data isn't one
// contiguous range.
// ---------------------------------------------------------------------------

void StreamingRemuxSession::beginFragmentDiscovery(TrackHead &track, bool isVideoTrack)
{
    // Precompute each fragment's absolute source offset up front from
    // sidx (a simple running sum) so requests can be dispatched
    // out-of-order without needing a single sequential cursor.
    std::vector<uint64_t> &offsets = isVideoTrack ? m_videoFragOffsets : m_audioFragOffsets;
    offsets.clear();
    offsets.reserve(track.sidx.entries.size());
    uint64_t running = uint64_t(track.sidx.firstFragmentOffset);
    for (size_t i = 0; i < track.sidx.entries.size(); i++) {
        offsets.push_back(running);
        running += track.sidx.entries[i].referencedSize;
    }

    if (isVideoTrack) {
        m_videoFragDiscoveryIndex = 0;
        m_videoFragDiscoveryCompleted = 0;
    } else {
        m_audioFragDiscoveryIndex = 0;
        m_audioFragDiscoveryCompleted = 0;
    }

    // Reserve space in fragSamples-per-fragment storage: we can't append
    // directly to track.fragSamples as requests complete since they don't
    // finish in order, so stash each fragment's samples in a slot indexed
    // by fragment number and concatenate once every fragment is in.
    track.fragSamples.clear(); // will be rebuilt from the per-fragment slots

    qDebug() << "[bbtube][remux][debug] beginFragmentDiscovery" << (isVideoTrack ? "video" : "audio")
             << "sidx.found=" << track.sidx.found
             << "sidx.firstFragmentOffset=" << qint64(track.sidx.firstFragmentOffset)
             << "sidx.entries.size()=" << qint64(track.sidx.entries.size());
    for (size_t i = 0; i < track.sidx.entries.size() && i < 5; i++) {
        qDebug() << "[bbtube][remux][debug]   sidx.entries[" << qint64(i) << "] referencedSize="
                 << qint64(track.sidx.entries[i].referencedSize) << "subsegmentDuration="
                 << qint64(track.sidx.entries[i].subsegmentDuration);
    }

    if (track.sidx.entries.empty()) {
        failWith(QString("%1: sidx box has no fragment entries")
                .arg(isVideoTrack ? "video" : "audio"));
        return;
    }

    if (isVideoTrack) {
        m_videoFragSlots.assign(track.sidx.entries.size(), std::vector<FragSample>());
    } else {
        m_audioFragSlots.assign(track.sidx.entries.size(), std::vector<FragSample>());
    }

    dispatchMoofRequests(isVideoTrack);
}

// Keeps up to MOOF_DISCOVERY_CONCURRENCY moof requests in flight for this
// track at once. Called both to kick off the initial batch and again
// after each completion to keep the window full until every fragment has
// been dispatched.
void StreamingRemuxSession::dispatchMoofRequests(bool isVideoTrack)
{
    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    size_t &nextIdx = isVideoTrack ? m_videoFragDiscoveryIndex : m_audioFragDiscoveryIndex;
    QList<QNetworkReply *> &inFlight = isVideoTrack ? m_videoMoofReplies : m_audioMoofReplies;
    const std::vector<uint64_t> &offsets = isVideoTrack ? m_videoFragOffsets : m_audioFragOffsets;
    QString url = isVideoTrack ? m_videoUrl : m_audioUrl;

    while (inFlight.size() < MOOF_DISCOVERY_CONCURRENCY && nextIdx < track.sidx.entries.size()) {
        uint64_t offset = offsets[nextIdx];
        QNetworkRequest req(QUrl::fromEncoded(url.toUtf8()));
        qint64 rangeEnd = qint64(offset) + MOOF_DISCOVERY_FETCH_BYTES - 1;
        QByteArray rangeHeader = "bytes=" + QByteArray::number(qint64(offset)) + "-"
                + QByteArray::number(rangeEnd);
        req.setRawHeader("Range", rangeHeader);

        qDebug() << "[bbtube][remux][debug] dispatchMoofRequests" << (isVideoTrack ? "video" : "audio")
                 << "fragIndex=" << qint64(nextIdx) << "offset=" << qint64(offset)
                 << "range=" << rangeHeader << "inFlight=" << inFlight.size() + 1;

        QNetworkReply *reply = m_networkManager->get(req);
        // fragIndex travels with the reply itself (rather than being
        // inferred from completion order, which is no longer meaningful
        // once several requests are in flight at once).
        reply->setProperty("fragIndex", qint64(nextIdx));
        inFlight.append(reply);
        if (isVideoTrack) {
            QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoMoofFinished()));
        } else {
            QObject::connect(reply, SIGNAL(finished()), this, SLOT(onAudioMoofFinished()));
        }
        nextIdx++;
    }

    // All fragments dispatched and none still in flight -- but only once
    // every fragment has actually COMPLETED (not just been dispatched) is
    // discovery really done; onVideoMoofFinished()/onAudioMoofFinished()
    // check that and call onTrackFragmentsReady() themselves once the
    // completed count catches up, so there's nothing further to do here
    // in that case.
}

void StreamingRemuxSession::onVideoMoofFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    m_videoMoofReplies.removeOne(reply);
    qint64 fragIndex = reply->property("fragIndex").toLongLong();

    qDebug() << "[bbtube][remux][debug] onVideoMoofFinished fragIndex=" << fragIndex
             << "error=" << int(reply->error()) << "errorString=" << reply->errorString()
             << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
             << "bytesAvailable=" << reply->bytesAvailable() << "url=" << reply->url().toString();

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith(QString("video fragment %1 header fetch failed: %2").arg(fragIndex).arg(msg));
        return;
    }
    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    uint64_t fragOffset = m_videoFragOffsets[size_t(fragIndex)];
    try {
        m_videoFragSlots[size_t(fragIndex)] = parseMoofSamples(buf, size_t(fragOffset), "video");
    } catch (const std::exception &e) {
        failWith(QString("video fragment %1 moof parse failed: %2").arg(fragIndex).arg(e.what()));
        return;
    }

    m_videoFragDiscoveryCompleted++;
    if (m_videoFragDiscoveryCompleted >= m_videoHead.sidx.entries.size()) {
        // Every fragment's samples are in (regardless of completion
        // order) -- concatenate the per-fragment slots back into
        // fragSamples in fragment order and move on.
        m_videoHead.fragSamples.clear();
        for (size_t i = 0; i < m_videoFragSlots.size(); i++) {
            m_videoHead.fragSamples.insert(m_videoHead.fragSamples.end(),
                    m_videoFragSlots[i].begin(), m_videoFragSlots[i].end());
        }
        onTrackFragmentsReady(true);
        return;
    }
    dispatchMoofRequests(true);
}

void StreamingRemuxSession::onAudioMoofFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    m_audioMoofReplies.removeOne(reply);
    qint64 fragIndex = reply->property("fragIndex").toLongLong();

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith(QString("audio fragment %1 header fetch failed: %2").arg(fragIndex).arg(msg));
        return;
    }
    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    uint64_t fragOffset = m_audioFragOffsets[size_t(fragIndex)];
    try {
        m_audioFragSlots[size_t(fragIndex)] = parseMoofSamples(buf, size_t(fragOffset), "audio");
    } catch (const std::exception &e) {
        failWith(QString("audio fragment %1 moof parse failed: %2").arg(fragIndex).arg(e.what()));
        return;
    }

    m_audioFragDiscoveryCompleted++;
    if (m_audioFragDiscoveryCompleted >= m_audioHead.sidx.entries.size()) {
        m_audioHead.fragSamples.clear();
        for (size_t i = 0; i < m_audioFragSlots.size(); i++) {
            m_audioHead.fragSamples.insert(m_audioHead.fragSamples.end(),
                    m_audioFragSlots[i].begin(), m_audioFragSlots[i].end());
        }
        onTrackFragmentsReady(false);
        return;
    }
    dispatchMoofRequests(false);
}

void StreamingRemuxSession::onTrackFragmentsReady(bool isVideoTrack)
{
    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    try {
        buildProgressiveTablesFromFragments(track);
    } catch (const std::exception &e) {
        failWith(QString("%1: failed building sample table from fragments: %2")
                .arg(isVideoTrack ? "video" : "audio").arg(e.what()));
        return;
    }

    if (isVideoTrack) {
        m_videoFragmentsReady = true;
        m_videoHeadParsed = true;
    } else {
        m_audioFragmentsReady = true;
        m_audioHeadParsed = true;
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux]" << (isVideoTrack ? "video" : "audio")
             << "fragment discovery complete -" << qint64(track.fragSamples.size())
             << "samples across" << qint64(track.sidx.entries.size()) << "fragments";
#endif

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

    if (m_videoHead.isFragmented || m_audioHead.isFragmented) {
        beginFragmentedBodyDownloads();
    } else {
        beginBodyDownloads();
    }
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

// ---------------------------------------------------------------------------
// Fragment-by-fragment body streaming (fragmented sources only). Unlike
// the contiguous-mdat path above (one long Range request per track), each
// sample of a fragmented track was recorded during discovery with its own
// absolute source offset (FragSample::offsetInSource) and its own output
// position was assigned by planStreamingRemux()'s stco patching -- so here
// we walk track.fragSamples in order, one small Range request per sample,
// writing each straight to its final output offset. This is chattier than
// the old one-request-per-track approach, but samples for adaptiveFormats
// video are typically dozens of KB each (audio samples smaller), so the
// request overhead stays proportionally small next to the payload.
// ---------------------------------------------------------------------------

void StreamingRemuxSession::beginFragmentedBodyDownloads()
{
    m_outputFile = new QFile(m_outputPath);
    if (!m_outputFile->open(QIODevice::ReadWrite)) {
        failWith("could not reopen output file for streaming writes: " + m_outputPath);
        return;
    }

    m_videoFragBodyIndex = 0;
    m_audioFragBodyIndex = 0;
    m_videoFragBodyOffsetSoFar = 0;
    m_audioFragBodyOffsetSoFar = 0;
    requestNextFragBody(true);
    requestNextFragBody(false);
}

void StreamingRemuxSession::requestNextFragBody(bool isVideoTrack)
{
    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    size_t &idx = isVideoTrack ? m_videoFragBodyIndex : m_audioFragBodyIndex;

    if (idx >= track.fragSamples.size()) {
        if (isVideoTrack) { m_videoDone = true; } else { m_audioDone = true; emit audioComplete(); }
        checkAllDone();
        return;
    }

    const FragSample &sample = track.fragSamples[idx];
    QString url = isVideoTrack ? m_videoUrl : m_audioUrl;
    QNetworkRequest req(QUrl::fromEncoded(url.toUtf8()));
    qint64 rangeStart = qint64(sample.offsetInSource);
    qint64 rangeEnd = rangeStart + qint64(sample.size) - 1;
    req.setRawHeader("Range",
            "bytes=" + QByteArray::number(rangeStart) + "-" + QByteArray::number(rangeEnd));

    QNetworkReply *reply = m_networkManager->get(req);
    if (isVideoTrack) {
        m_videoFragBodyReply = reply;
        QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoFragBodyFinished()));
    } else {
        m_audioFragBodyReply = reply;
        QObject::connect(reply, SIGNAL(finished()), this, SLOT(onAudioFragBodyFinished()));
    }
}

void StreamingRemuxSession::onVideoFragBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_videoFragBodyReply;
    m_videoFragBodyReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("video fragment sample download failed: " + msg);
        return;
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();

    const FragSample &sample = m_videoHead.fragSamples[m_videoFragBodyIndex];
    if (quint64(body.size()) != sample.size) {
        failWith(QString("video sample %1 size mismatch: expected %2, got %3 -- server may "
                "not have honored the Range request").arg(m_videoFragBodyIndex).arg(
                sample.size).arg(body.size()));
        return;
    }

    // Output position for this sample: videoOutputOffset (start of this
    // track's span in the output file) plus the running total of all
    // prior video samples' sizes, tracked incrementally to keep each
    // write O(1) rather than re-summing every prior sample.
    qint64 outOffset = qint64(m_plan.videoOutputOffset) + m_videoFragBodyOffsetSoFar;

    if (!m_outputFile->seek(outOffset) || m_outputFile->write(body) != body.size()) {
        failWith("failed writing video sample to output file");
        return;
    }

    m_videoFragBodyOffsetSoFar += body.size();
    m_videoBytesWritten += body.size();
    emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));

    m_videoFragBodyIndex++;
    requestNextFragBody(true);
}

void StreamingRemuxSession::onAudioFragBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioFragBodyReply;
    m_audioFragBodyReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("audio fragment sample download failed: " + msg);
        return;
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();

    const FragSample &sample = m_audioHead.fragSamples[m_audioFragBodyIndex];
    if (quint64(body.size()) != sample.size) {
        failWith(QString("audio sample %1 size mismatch: expected %2, got %3 -- server may "
                "not have honored the Range request").arg(m_audioFragBodyIndex).arg(
                sample.size).arg(body.size()));
        return;
    }

    qint64 outOffset = qint64(m_plan.audioOutputOffset) + m_audioFragBodyOffsetSoFar;

    if (!m_outputFile->seek(outOffset) || m_outputFile->write(body) != body.size()) {
        failWith("failed writing audio sample to output file");
        return;
    }

    m_audioFragBodyOffsetSoFar += body.size();
    m_audioFragBodyIndex++;
    requestNextFragBody(false);
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
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    m_videoMoofReplies.clear();
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    m_audioMoofReplies.clear();
    if (m_videoFragBodyReply) { m_videoFragBodyReply->abort(); m_videoFragBodyReply->deleteLater(); m_videoFragBodyReply = 0; }
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); m_audioFragBodyReply = 0; }
    if (m_outputFile) { m_outputFile->close(); }

    emit failed(message);
}
