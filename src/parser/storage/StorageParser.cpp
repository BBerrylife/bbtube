#include "StorageParser.hpp"
#include "src/parser/models/StorageData.hpp"

#include <bb/data/JsonDataAccess>
#include <QUrl>

// StorageParser: turns a player-response JSON payload (from either the
// legacy ytplayer.config blob or the modern InnerTube /player endpoint)
// into a StorageData object with playable URLs.
//
// KEY CHANGES vs. the original (2021) implementation:
//  - "playabilityStatus" is now checked first: if status != "OK" we bail out
//    early (age-gated / unavailable / login-required videos used to silently
//    return zero formats, causing a confusing "no instances" error downstream).
//  - YouTube removed the combined progressive "formats" array's signed URLs
//    for many videos; almost everything now arrives via "adaptiveFormats"
//    with either a direct "url" or a "signatureCipher"/"cipher" field.
//  - Live streams now expose "dashManifestUrl" in addition to
//    "hlsManifestUrl"; we prefer HLS for player compatibility.
//  - Caption track keys changed slightly; "vssId" can be absent so we now
//    fall back to "languageCode".

void StorageParser::parseFromHtml(StorageData *storageData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(*json);
    QVariantMap map = parsed.toMap();
    QString playerResponse = map["args"].toMap()["player_response"].toString();
    QVariantMap playerResponseMap = jda.loadFromBuffer(playerResponse).toMap();

    parseFromJsonInternal(storageData, &playerResponseMap);
}

void StorageParser::parseFromJson(StorageData *storageData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(*json);
    QVariantMap playerResponseMap = parsed.toMap();

    parseFromJsonInternal(storageData, &playerResponseMap);
}

void StorageParser::parseFromJsonInternal(StorageData *storageData,
        const QVariantMap *playerResponseMap)
{
    // ── Check playability status first ──────────────────────────────────────
    QVariantMap playabilityStatus = (*playerResponseMap)["playabilityStatus"].toMap();
    QString status = playabilityStatus["status"].toString();

    if (!status.isEmpty() && status != "OK" && status != "LIVE_STREAM") {
        // LOGIN_REQUIRED, ERROR, UNPLAYABLE, AGE_CHECK_REQUIRED, etc.
        // Leave storageData empty; caller (YoutubeClient) surfaces this as
        // "Source unavailable" via the existing error path.
        return;
    }

    QVariantMap streamingData = (*playerResponseMap)["streamingData"].toMap();

    // ── Live stream handling ────────────────────────────────────────────────
    if (streamingData.contains("hlsManifestUrl")) {
        SingleVideoStorageData video;
        video.url = QUrl::fromPercentEncoding(streamingData["hlsManifestUrl"].toByteArray());
        video.duration = 0;
        video.quality = "live";
        storageData->instances.append(video);
        return;
    }
    if (streamingData.contains("dashManifestUrl")) {
        SingleVideoStorageData video;
        video.url = QUrl::fromPercentEncoding(streamingData["dashManifestUrl"].toByteArray());
        video.duration = 0;
        video.quality = "live";
        storageData->instances.append(video);
        return;
    }

    // ── Progressive (video+audio combined) formats ─────────────────────────
    QVariantList formats = streamingData["formats"].toList();

    for (int i = 0; i < formats.count(); i++) {
        QVariantMap format = formats[i].toMap();
        SingleVideoStorageData video;

        video.quality = format["qualityLabel"].toString();
        video.duration = format["approxDurationMs"].toInt();
        video.contentLength = format["contentLength"].toString().toULongLong();

        if (format.contains("url")) {
            video.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
        } else if (format.contains("signatureCipher")) {
            video.cipher = format["signatureCipher"].toString();
        } else if (format.contains("cipher")) {
            video.cipher = format["cipher"].toString();
        }

        if (video.url.isEmpty() && video.cipher.isEmpty()) {
            continue; // unusable entry, skip
        }

        storageData->instances.append(video);
    }

    // ── Adaptive formats: video-only AND audio-only streams ────────────────
    // Modern YouTube puts almost everything here. Progressive "formats" is
    // often empty/limited (sometimes capped at 720p or missing entirely for
    // certain client contexts), so adaptiveFormats is now the primary source
    // for both audio and (if needed) video-only fallback.
    QVariantList adaptiveFormats = streamingData["adaptiveFormats"].toList();

    bool haveAudio = false;

    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap format = adaptiveFormats[i].toMap();
        QString mimeType = format["mimeType"].toString();

        bool isAudio = mimeType.startsWith("audio/");
        bool isVideo = mimeType.startsWith("video/");

        if (isAudio) {
            // Prefer mp4/m4a audio (AAC) for widest device compatibility;
            // fall back to whatever is available if no mp4 audio exists.
            bool isMp4Audio = mimeType.contains("audio/mp4");

            if (haveAudio && !isMp4Audio) {
                continue; // keep first/best match
            }

            AudioStorageData audio;

            if (format.contains("url")) {
                audio.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
            } else if (format.contains("signatureCipher")) {
                audio.cipher = format["signatureCipher"].toString();
            } else if (format.contains("cipher")) {
                audio.cipher = format["cipher"].toString();
            }

            audio.contentLength = format["contentLength"].toString().toULongLong();

            if (!audio.url.isEmpty() || !audio.cipher.isEmpty()) {
                storageData->audio = audio;
                haveAudio = isMp4Audio || haveAudio;
                if (!haveAudio) haveAudio = true;
            }
        } else if (isVideo && storageData->instances.isEmpty()) {
            // Only used as a fallback if "formats" gave us nothing at all
            // (some restricted/age-gated or new uploads only expose
            // adaptiveFormats). These are video-only streams (no embedded
            // audio track) but better than nothing for playback.
            if (!mimeType.contains("video/mp4")) {
                continue;
            }

            SingleVideoStorageData video;
            video.quality = format["qualityLabel"].toString();
            video.duration = format["approxDurationMs"].toInt();
            video.contentLength = format["contentLength"].toString().toULongLong();

            if (format.contains("url")) {
                video.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
            } else if (format.contains("signatureCipher")) {
                video.cipher = format["signatureCipher"].toString();
            } else if (format.contains("cipher")) {
                video.cipher = format["cipher"].toString();
            }

            if (!video.url.isEmpty() || !video.cipher.isEmpty()) {
                storageData->instances.append(video);
            }
        }
    }

    // ── Closed captions ──────────────────────────────────────────────────
    QVariantList captionList = (*playerResponseMap)["captions"].toMap()
            ["playerCaptionsTracklistRenderer"].toMap()["captionTracks"].toList();

    for (int i = 0; i < captionList.count(); i++) {
        QVariantMap captionMap = captionList[i].toMap();
        ClosedCaptionData cc;

        cc.isLoaded = false;

        QString vssId = captionMap["vssId"].toString();
        if (!vssId.isEmpty()) {
            cc.languageCode = vssId.replace(".", "");
        } else {
            cc.languageCode = captionMap["languageCode"].toString();
        }

        // "name" may be {"simpleText": "..."} or {"runs": [{"text": "..."}]}
        QVariantMap nameMap = captionMap["name"].toMap();
        if (nameMap.contains("simpleText")) {
            cc.languageName = nameMap["simpleText"].toString();
        } else if (nameMap.contains("runs")) {
            QVariantList runs = nameMap["runs"].toList();
            QString text;
            for (int r = 0; r < runs.count(); r++) {
                text += runs[r].toMap()["text"].toString();
            }
            cc.languageName = text;
        }

        cc.url = captionMap["baseUrl"].toString() + "&fmt=ttml";

        storageData->captions.append(cc);
    }
}
