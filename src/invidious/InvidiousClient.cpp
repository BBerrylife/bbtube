#include "InvidiousClient.hpp"
#include "src/applicationui.hpp"

#include <bb/data/JsonDataAccess>

#include <QtNetwork/QNetworkRequest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QDebug>
#include <QtNetwork/QSslCertificate>
#include <QTimer>

InvidiousClient::InvidiousClient(InvidiousInstanceManager *instanceManager, QObject *parent) :
        QObject(parent), instanceManager(instanceManager)
{
}

void InvidiousClient::fetchVideo(const QString &videoId)
{
    QString instanceBaseUrl = instanceManager->pickRandomInstance();
    if (instanceBaseUrl.isEmpty()) {
        emit error("No Invidious instance available");
        return;
    }
    requestFromInstance(videoId, instanceBaseUrl, 1);
}

void InvidiousClient::requestFromInstance(const QString &videoId, const QString &instanceBaseUrl,
        int attemptNumber)
{
    qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "requesting" << videoId
             << "from" << instanceBaseUrl;

    QNetworkRequest request(
            instanceBaseUrl + "/api/v1/videos/" + videoId + "?local=true");
    request.setRawHeader("User-Agent", "bbtube (BlackBerry 10)");

    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    reply->setProperty("videoId", videoId);
    reply->setProperty("attemptNumber", attemptNumber);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoRequestFinished()));
    QObject::connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this,
            SLOT(onSslErrors(QList<QSslError>)));

    // Same rationale as InvidiousInstanceManager::refreshInstanceList():
    // no built-in request timeout on this Qt4 build, so without this a
    // stalled handshake against one bad instance would hang the video
    // load indefinitely (the "spins forever, never gets to the player"
    // symptom) instead of failing fast and retrying the next instance.
    QTimer::singleShot(10000, reply, SLOT(abort()));
}

void InvidiousClient::onSslErrors(const QList<QSslError> &errors)
{
    for (int i = 0; i < errors.count(); i++) {
        qDebug() << "[bbtube][invidious][ssl] error" << i << ":" << errors[i].errorString()
                 << "(code" << errors[i].error() << ")";
        QSslCertificate cert = errors[i].certificate();
        if (!cert.isNull()) {
            qDebug() << "[bbtube][invidious][ssl]   certificate subject:"
                     << cert.subjectInfo(QSslCertificate::CommonName) << ", issuer:"
                     << cert.issuerInfo(QSslCertificate::CommonName) << ", expires:"
                     << cert.expiryDate().toString();
        }
    }
}

void InvidiousClient::onVideoRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    reply->deleteLater();

    QString videoId = reply->property("videoId").toString();
    int attemptNumber = reply->property("attemptNumber").toInt();
    QVariant httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

    bool networkOk = reply->error() == QNetworkReply::NoError;
    QString body = QString::fromUtf8(reply->readAll());

    qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "response for" << videoId
             << "-- url:" << reply->url().toString()
             << ", network error code:" << reply->error()
             << ", network error string:" << reply->errorString()
             << ", http status:" << httpStatus
             << ", body length:" << body.length();

    QVariantMap videoMap;
    bool parsedOk = false;
    if (networkOk && !body.isEmpty()) {
        bb::data::JsonDataAccess jda;
        QVariant parsed = jda.loadFromBuffer(body);
        videoMap = parsed.toMap();
        // A successful Invidious video response always has a non-empty
        // "videoId" field; an error response instead has an "error"
        // string field and no videoId. Use that as our parse-succeeded
        // check rather than trusting the HTTP status code alone (some
        // instances return HTTP 200 with a JSON error body).
        parsedOk = !videoMap["videoId"].toString().isEmpty();

        if (!parsedOk) {
            // Either malformed JSON, or a well-formed Invidious error
            // response like {"error": "..."} -- log which, and the
            // instance's own error message if present, rather than just
            // "failed" with no explanation.
            QString instanceError = videoMap["error"].toString();
            if (!instanceError.isEmpty()) {
                qDebug() << "[bbtube][invidious] attempt" << attemptNumber
                         << "instance reported error:" << instanceError;
            } else {
                qDebug() << "[bbtube][invidious] attempt" << attemptNumber
                         << "response body did not parse as a valid video JSON. First 200 chars:"
                         << body.left(200);
            }
        }
    }

    bool usable = parsedOk && hasUsableStreams(videoMap);

    if (parsedOk && !usable) {
        qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "for" << videoId
                 << "-- video JSON parsed OK but had no usable formatStreams/adaptiveFormats URLs"
                    " (likely SABR-blocked on this instance too)";
    }

    if (!usable) {
        qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "for" << videoId
                  << "failed or had no usable streams"
                  << (networkOk ? "" : QString("(network error: %1)").arg(reply->errorString()));

        if (attemptNumber < MAX_INSTANCE_ATTEMPTS) {
            QString nextInstance = instanceManager->pickRandomInstance();
            if (!nextInstance.isEmpty()) {
                qDebug() << "[bbtube][invidious] retrying with instance:" << nextInstance;
                requestFromInstance(videoId, nextInstance, attemptNumber + 1);
                return;
            } else {
                qDebug() << "[bbtube][invidious] no next instance available to retry with";
            }
        } else {
            int maxAttempts = MAX_INSTANCE_ATTEMPTS;
            qDebug() << "[bbtube][invidious] reached MAX_INSTANCE_ATTEMPTS (" << maxAttempts
                     << "), giving up on Invidious for" << videoId;
        }

        emit error("Could not get a playable stream from any Invidious instance");
        return;
    }

    VideoMetadata metadata;
    mapToVideoMetadata(videoMap, &metadata);

    StorageData storageData;
    mapToStorageData(videoMap, &storageData);

    emit metadataReceived(metadata, storageData);
}

bool InvidiousClient::hasUsableStreams(const QVariantMap &videoMap)
{
    QVariantList formatStreams = videoMap["formatStreams"].toList();
    for (int i = 0; i < formatStreams.count(); i++) {
        if (!formatStreams[i].toMap()["url"].toString().isEmpty()) {
            return true;
        }
    }

    QVariantList adaptiveFormats = videoMap["adaptiveFormats"].toList();
    for (int i = 0; i < adaptiveFormats.count(); i++) {
        if (!adaptiveFormats[i].toMap()["url"].toString().isEmpty()) {
            return true;
        }
    }

    return false;
}

void InvidiousClient::mapToVideoMetadata(const QVariantMap &videoMap, VideoMetadata *outMetadata)
{
    SingleVideoMetadata video;
    video.title = videoMap["title"].toString();
    video.videoId = videoMap["videoId"].toString();
    video.channelId = videoMap["authorId"].toString();
    video.channelTitle = videoMap["author"].toString();
    // Invidious gives lengthSeconds as an integer, but SingleVideoMetadata
    // (shared with the InnerTube path) stores a pre-formatted lengthText
    // and treats "" as "this is a live stream" (see
    // SingleVideoMetadata::isLiveStream()). We don't need a fully
    // formatted "H:MM:SS" here -- lengthText is only read by
    // isLiveStream() and a couple of list-item labels elsewhere -- so a
    // simple non-empty placeholder is enough to mark this as non-live;
    // liveNow (checked separately, see mapToStorageData) is what actually
    // drives live-stream handling in StorageData/PlayerPage.
    bool liveNow = videoMap["liveNow"].toBool();
    video.lengthText = liveNow ? "" : QString::number(videoMap["lengthSeconds"].toInt());
    video.viewsCount = QString::number(videoMap["viewCount"].toLongLong());
    video.shortViewsCount = video.viewsCount;
    video.dateUploadedAgo = videoMap["publishedText"].toString();

    QVariantList thumbnails = videoMap["videoThumbnails"].toList();
    if (!thumbnails.isEmpty()) {
        // Thumbnails are listed largest-first in Invidious's schema; take
        // a mid-sized one if available, else whatever's there.
        int idx = thumbnails.count() > 2 ? thumbnails.count() / 2 : 0;
        video.thumbnailUrl = thumbnails[idx].toMap()["url"].toString();
    }

    outMetadata->video = video;

    QVariantList recommended = videoMap["recommendedVideos"].toList();
    for (int i = 0; i < recommended.count(); i++) {
        QVariantMap rec = recommended[i].toMap();
        SingleVideoMetadata relatedVideo;
        relatedVideo.videoId = rec["videoId"].toString();
        relatedVideo.title = rec["title"].toString();
        relatedVideo.channelTitle = rec["author"].toString();
        relatedVideo.shortViewsCount = rec["viewCountText"].toString();
        relatedVideo.viewsCount = relatedVideo.shortViewsCount;
        relatedVideo.lengthText = QString::number(rec["lengthSeconds"].toInt());

        QVariantList recThumbnails = rec["videoThumbnails"].toList();
        if (!recThumbnails.isEmpty()) {
            int idx = recThumbnails.count() > 2 ? recThumbnails.count() / 2 : 0;
            relatedVideo.thumbnailUrl = recThumbnails[idx].toMap()["url"].toString();
        }

        outMetadata->relatedVideos.otherVideos.append(relatedVideo);
    }
    if (!outMetadata->relatedVideos.otherVideos.isEmpty()) {
        outMetadata->relatedVideos.nextVideo = outMetadata->relatedVideos.otherVideos[0];
    }
}

// Ascending-quality comparator for qSort, matching the order
// PlayerPage::getIndexOfDefaultQuality() expects (it does a plain
// QString::compare walk assuming instances[] is sorted low-to-high by
// quality label, e.g. "144p" < "240p" < ... < "720p60"). String comparison
// alone isn't numerically correct ("1080p" < "720p" alphabetically!), so
// we extract the leading number from each label to compare numerically,
// falling back to string comparison only if that fails.
static int qualityLabelToNumber(const QString &label)
{
    QString digits;
    for (int i = 0; i < label.length(); i++) {
        if (label[i].isDigit()) {
            digits += label[i];
        } else if (!digits.isEmpty()) {
            break; // stop at the first non-digit run after we've seen digits (handles "720p60" -> "720")
        }
    }
    bool ok = false;
    int value = digits.toInt(&ok);
    return ok ? value : 0;
}

static bool singleVideoStorageDataLessThan(const SingleVideoStorageData &a,
        const SingleVideoStorageData &b)
{
    return qualityLabelToNumber(a.quality) < qualityLabelToNumber(b.quality);
}

void InvidiousClient::mapToStorageData(const QVariantMap &videoMap, StorageData *outStorageData)
{
    bool liveNow = videoMap["liveNow"].toBool();

    if (liveNow) {
        QString hlsUrl = videoMap["hlsUrl"].toString();
        if (!hlsUrl.isEmpty()) {
            SingleVideoStorageData live;
            live.url = hlsUrl;
            live.quality = "live";
            live.hasEmbeddedAudio = true;
            outStorageData->instances.append(live);
        }
        return;
    }

    // Progressive (audio+video combined) streams -- these can be played
    // directly, no remux needed. Matches StorageParser's handling of
    // InnerTube's "formats" array (see hasEmbeddedAudio comment in
    // StorageData.hpp).
    QVariantList formatStreams = videoMap["formatStreams"].toList();
    for (int i = 0; i < formatStreams.count(); i++) {
        QVariantMap fmt = formatStreams[i].toMap();
        QString url = fmt["url"].toString();
        if (url.isEmpty()) {
            continue;
        }
        SingleVideoStorageData instance;
        instance.url = url;
        instance.quality = fmt["qualityLabel"].toString();
        instance.hasEmbeddedAudio = true;
        outStorageData->instances.append(instance);
    }

    // Adaptive (video-only) streams -- highest-quality options, but need
    // pairing with a separate audio track and remuxing (see
    // StreamingRemuxSession) before BB10's mmrenderer can play them.
    QVariantList adaptiveFormats = videoMap["adaptiveFormats"].toList();
    QString bestAudioUrl;
    unsigned long long bestAudioBitrate = 0;

    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap fmt = adaptiveFormats[i].toMap();
        QString url = fmt["url"].toString();
        if (url.isEmpty()) {
            continue;
        }
        QString type = fmt["type"].toString(); // e.g. "video/mp4; codecs=\"avc1.4d401f\""

        if (type.startsWith("audio/")) {
            // Pick the highest-bitrate audio track available, matching
            // StorageParser's approach for the InnerTube path.
            unsigned long long bitrate = fmt["bitrate"].toString().toULongLong();
            if (bitrate >= bestAudioBitrate) {
                bestAudioBitrate = bitrate;
                bestAudioUrl = url;
            }
            continue;
        }

        if (!type.startsWith("video/")) {
            continue;
        }
        // Only H.264 (avc1) mp4 video is usable by BB10's mmrenderer /
        // this app's remuxer -- skip VP9/AV1/webm, matching
        // StorageParser's existing H.264-only filtering for the
        // InnerTube path.
        if (!type.contains("mp4") || !type.contains("avc1")) {
            continue;
        }

        QString qualityLabel = fmt["qualityLabel"].toString();
        if (qualityLabel.isEmpty()) {
            continue;
        }

        SingleVideoStorageData instance;
        instance.url = url;
        instance.quality = qualityLabel;
        instance.hasEmbeddedAudio = false;
        instance.contentLength = fmt["clen"].toString().toULongLong();
        outStorageData->instances.append(instance);
    }

    if (!bestAudioUrl.isEmpty()) {
        outStorageData->audio.url = bestAudioUrl;
        outStorageData->audio.contentLength = 0; // not surfaced separately by Invidious's schema per-track here
    }

    qSort(outStorageData->instances.begin(), outStorageData->instances.end(),
            singleVideoStorageDataLessThan);
}
