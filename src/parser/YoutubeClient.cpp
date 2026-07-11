#include "YoutubeClient.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/parser/models/RecommendedData.hpp"
#include "src/parser/models/TrendingData.hpp"

#include "src/applicationui.hpp"
#include "src/parser/script/ScriptData.hpp"
#include "src/parser/script/ScriptParser.hpp"
#include "src/parser/cipher/DecryptHelper.hpp"
#include "src/parser/storage/StorageParser.hpp"
#include "src/parser/models/SearchData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"
#include "src/parser/search/SuggestionsParser.hpp"
#include "src/parser/channel/ChannelPageParser.hpp"
#include "src/parser/recommended/RecommendedPageParser.hpp"
#include "src/parser/trending/TrendingPageParser.hpp"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkCookieJar>
#include <QtCore/QDate>

QMap<QString, ScriptData> YoutubeClient::cachedScripts;

// Current InnerTube client context (WEB client, up-to-date version)
// UPDATE THIS VERSION periodically - check https://www.youtube.com and search for "INNERTUBE_CLIENT_VERSION"
static const QString INNERTUBE_CLIENT_VERSION = "2.20240101.00.00";
static const QString INNERTUBE_CLIENT_NAME    = "WEB";
static const QString INNERTUBE_API_URL_BASE   = "https://www.youtube.com/youtubei/v1/";

// REQUIRED: every InnerTube request must include a ?key= query parameter
// matching the calling client, or the server replies "400 Bad Request"
// before even looking at the JSON body. These are long-standing public
// client keys embedded in YouTube's own web/Android clients (not secrets).
static const QString INNERTUBE_API_KEY_WEB     = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
static const QString INNERTUBE_API_KEY_ANDROID = "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w";

// NOTE: request bodies are built inline via QString::arg() in each method
// below (parse/search/channel/recommended/trending) rather than through a
// shared helper, since each endpoint needs a slightly different context
// shape (e.g. the player endpoint uses the ANDROID client, others use WEB).

QString YoutubeClient::getVideoId(QString text)
{
    QRegExp videoIdRegExp(".*(?:youtu.be\\/|v\\/|u\\/\\w\\/|embed\\/|watch\\?v=|shorts\\/)([^#\\&\\?]*).*");
    videoIdRegExp.indexIn(text);

    if (videoIdRegExp.cap(1) != "") {
        return videoIdRegExp.cap(1);
    }

    QUrl url(text);
    if (!url.isValid()) {
        return "";
    }

    QString videoId = url.queryItemValue("v");
    if (videoId != "" && url.host().contains("youtube.com")) {
        return videoId;
    }

    return "";
}

void YoutubeClient::process(QString text)
{
    if (text == "") {
        return;
    }

    QString videoId = getVideoId(text);

    if (videoId != "") {
        parse(videoId);
        return;
    }

    search(text);
}

void YoutubeClient::parse(QString videoId)
{
    // Use InnerTube /player endpoint (POST) instead of fetching the watch page
    // This is far more reliable than scraping HTML and parsing ytInitialData
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "player?key=" + INNERTUBE_API_KEY_ANDROID + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"ANDROID\","
                "\"clientVersion\":\"19.09.37\","
                "\"androidSdkVersion\":30,"
                "\"hl\":\"en\","
                "\"gl\":\"US\""
            "}"
        "},"
        "\"videoId\":\"%1\","
        "\"params\":\"8AEB\""
        "}"
    ).arg(videoId);

    // Store videoId for use in onGetHtmlFinished
    QNetworkReply *playerReply = ApplicationUI::networkManager->post(request, body.toUtf8());
    playerReply->setProperty("videoId", videoId);
    QObject::connect(playerReply, SIGNAL(finished()), this, SLOT(onPlayerApiFinished()));

    // Also fetch the watch page to get ytInitialData for metadata & related videos
    QNetworkRequest watchRequest = prepareRequest("https://www.youtube.com/watch?v=" + videoId + "&hl=en");
    QNetworkReply *watchReply = ApplicationUI::networkManager->get(watchRequest);
    watchReply->setProperty("videoId", videoId);
    QObject::connect(watchReply, SIGNAL(finished()), this, SLOT(onGetHtmlFinished()));
}

void YoutubeClient::search(QString text)
{
    search(text, "");
}

void YoutubeClient::search(QString text, QString searchParams)
{
    // Use InnerTube /search endpoint (POST) — more stable than scraping HTML results
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "search?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body;
    if (searchParams.isEmpty()) {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"query\":\"%3\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, text);
    } else {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"query\":\"%3\","
            "\"params\":\"%4\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, text, searchParams);
    }

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onSearchFinished()));
}

void YoutubeClient::suggestions(QString text)
{
    if (text == "") {
        return;
    }

    // Suggestion endpoint unchanged
    QNetworkRequest request = prepareRequest(
        "https://suggestqueries-clients6.youtube.com/complete/search?client=youtube&ds=yt&q="
        + QUrl::toPercentEncoding(text));
    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onSuggestionsFinished()));
}

void YoutubeClient::channel(QString channelId, QString originalChannelId)
{
    // Use InnerTube /browse endpoint (POST) for channel pages
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "\"hl\":\"en\",\"gl\":\"US\""
            "}"
        "},"
        "\"browseId\":\"%3\","
        "\"params\":\"EgZ2aWRlb3PyBgQKAjoA\""
        "}"
    ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, channelId);

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    reply->setProperty("channelId", channelId);
    reply->setProperty("originalChannelId", originalChannelId);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onChannelFinished()));
}

void YoutubeClient::channelVideosNextBatch(ChannelPageData *channelData)
{
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", channelData->clientVersion.toUtf8());

    QByteArray data = QString(
        "{\"context\":{\"client\":{\"clientName\":\"%1\",\"clientVersion\":\"%2\"}},"
        "\"continuation\":\"%3\"}"
    ).arg(INNERTUBE_CLIENT_NAME, channelData->clientVersion, channelData->ctoken).toUtf8();

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, data);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onChannelVideosNextBatchFinished()));
}

void YoutubeClient::recommended()
{
    // Use InnerTube /browse?browseId=FEwhat_to_watch for the homepage/recommended feed
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "\"hl\":\"en\",\"gl\":\"US\""
            "}"
        "},"
        "\"browseId\":\"FEwhat_to_watch\""
        "}"
    ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION);

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onRecommendedFinished()));
}

void YoutubeClient::recommendedNextBatch(RecommendedData *recommendedData)
{
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", recommendedData->clientVersion.toUtf8());

    QByteArray data = QString(
        "{\"context\":{\"client\":{\"clientName\":\"%1\",\"clientVersion\":\"%2\"}},"
        "\"continuation\":\"%3\"}"
    ).arg(INNERTUBE_CLIENT_NAME, recommendedData->clientVersion, recommendedData->ctoken).toUtf8();

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, data);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onRecommendedNextBatchFinished()));
}

void YoutubeClient::trending(QString categoryKey)
{
    // Use InnerTube /browse?browseId=FEtrending for trending
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body;
    if (categoryKey.isEmpty()) {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"browseId\":\"FEtrending\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION);
    } else {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"browseId\":\"FEtrending\","
            "\"params\":\"%3\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, categoryKey);
    }

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onTrendingFinished()));
}

// ─── Slot: handle /player API response (stream URLs) ─────────────────────────
void YoutubeClient::onPlayerApiFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString videoId = reply->property("videoId").toString();
    QString httpErrorMessage;

    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        // Discard any metadata that arrived for this videoId — it would
        // otherwise sit in pendingVideoMetadata forever since
        // tryEmitMetadata() requires BOTH halves to be present.
        pendingVideoMetadata.remove(videoId);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    StorageData storageData;
    StorageParser::parseFromJson(&storageData, &response);

    // Decrypt any cipher-protected URLs.
    // NOTE: this check is intentionally NOT nested inside an
    // "instances.count() > 0" guard — a video can have zero progressive/
    // video-only instances but still have an audio-only cipher that needs
    // decrypting (e.g. some audio-only or restricted-format responses).
    bool needsJs = false;
    for (int i = 0; i < storageData.instances.count(); i++) {
        if (!storageData.instances[i].cipher.isEmpty()) {
            needsJs = true;
            break;
        }
    }
    if (!needsJs && !storageData.audio.cipher.isEmpty()) {
        needsJs = true;
    }

    if (needsJs) {
        // Fetch base.js to get cipher operations.
        // We fall back to fetching the watch page to get the script URL.
        QEventLoop loop;
        QNetworkRequest watchReq = prepareRequest("https://www.youtube.com/watch?v=" + videoId + "&hl=en");
        QNetworkReply *watchReply = ApplicationUI::networkManager->get(watchReq);
        QObject::connect(watchReply, SIGNAL(finished()), &loop, SLOT(quit()));
        loop.exec();

        QString watchHtml = QString(watchReply->readAll());
        watchReply->deleteLater();

        QString baseJsUrl = extractBaseJsUrl(watchHtml);
        if (!baseJsUrl.isEmpty() && !cachedScripts.contains(baseJsUrl)) {
            QNetworkReply *jsReply = ApplicationUI::networkManager->get(QNetworkRequest(baseJsUrl));
            QObject::connect(jsReply, SIGNAL(finished()), &loop, SLOT(quit()));
            loop.exec();
            onGetBaseJsFinished(jsReply);
        }

        if (!baseJsUrl.isEmpty() && cachedScripts.contains(baseJsUrl)) {
            ScriptData scriptData = cachedScripts[baseJsUrl];
            for (int i = 0; i < storageData.instances.count(); i++) {
                if (!storageData.instances[i].cipher.isEmpty()) {
                    storageData.instances[i].url = DecryptHelper::decryptUrl(
                        storageData.instances[i].cipher, &scriptData);
                }
            }
            if (!storageData.audio.cipher.isEmpty()) {
                storageData.audio.url = DecryptHelper::decryptUrl(
                    storageData.audio.cipher, &scriptData);
            }
        }
    }

    // Cache the storageData — the watch page handler will merge metadata + storageData
    pendingStorageData[videoId] = storageData;
    tryEmitMetadata(videoId);

    reply->deleteLater();
}

// ─── Slot: handle watch page HTML response (metadata & related videos) ────────
void YoutubeClient::onGetHtmlFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString requestedVideoId = reply->property("videoId").toString();

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        pendingStorageData.remove(requestedVideoId);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    QString json = getJson(response);

    VideoMetadata videoMetadata;
    ItemRendererParser::populateVideoMetadata(&videoMetadata, &json);

    if (videoMetadata.video.videoId.isEmpty()) {
        emit error("Source unavailable");
        // Use the videoId we requested with (parsing failed, so
        // videoMetadata.video.videoId is empty and can't be used as the key).
        pendingStorageData.remove(requestedVideoId);
        reply->deleteLater();
        return;
    }

    pendingVideoMetadata[videoMetadata.video.videoId] = videoMetadata;
    tryEmitMetadata(videoMetadata.video.videoId);

    reply->deleteLater();
}

// Emit metadataReceived only when BOTH player API response and watch page are ready
void YoutubeClient::tryEmitMetadata(const QString &videoId)
{
    if (pendingVideoMetadata.contains(videoId) && pendingStorageData.contains(videoId)) {
        VideoMetadata vm = pendingVideoMetadata.take(videoId);
        StorageData sd   = pendingStorageData.take(videoId);
        emit metadataReceived(vm, sd);
    }
}

// ─── Slot: handle InnerTube /search response ─────────────────────────────────
void YoutubeClient::onSearchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    SearchData searchData;
    ItemRendererParser::populateSearchData(&searchData, &response);
    emit searchDataReceived(searchData);

    reply->deleteLater();
}

void YoutubeClient::onSuggestionsFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    if (reply->error() || hasHttpError(reply, NULL)) {
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    // YouTube suggestions endpoint returns: window.google.ac.h([...])
    // or just a raw JSON array depending on the client param
    QStringList list;
    QString configKey = "window.google.ac.h(";
    int startOfConfig = response.indexOf(configKey);
    if (startOfConfig >= 0) {
        QString json = response.mid(startOfConfig + configKey.length(),
            response.length() - startOfConfig - configKey.length() - 1).trimmed();
        list = SuggestionsParser::parseSuggestions(&json);
    } else {
        // Try parsing as raw JSON array
        list = SuggestionsParser::parseSuggestions(&response);
    }

    emit suggestionsReceived(list);
    reply->deleteLater();
}

void YoutubeClient::onGetBaseJsFinished(QNetworkReply *reply)
{
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    ScriptData scriptData = ScriptParser::parse(response);
    cachedScripts.insert(reply->request().url().toString(), scriptData);
    reply->deleteLater();
}

void YoutubeClient::onChannelFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());

    ChannelPageData channelData;
    // clientVersion now extracted from the InnerTube response itself
    channelData.clientVersion = INNERTUBE_CLIENT_VERSION;

    if (reply->property("originalChannelId").toString() != "") {
        channelData.channelId = reply->property("originalChannelId").toString();
    } else {
        channelData.channelId = reply->property("channelId").toString();
    }

    ChannelPageParser::parse(&channelData, &response);

    if (!channelData.redirectChannelId.isEmpty()) {
        channel(channelData.redirectChannelId, channelData.channelId);
        reply->deleteLater();
        return;
    }

    if (!channelData.title.isEmpty()) {
        emit channelDataReceived(channelData);
    } else {
        emit error("Channel not found");
    }

    reply->deleteLater();
}

void YoutubeClient::onChannelVideosNextBatchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    ChannelPageData channelData;
    ChannelPageParser::parseNextBatch(&channelData, &response);
    emit channelVideosNextBatchReceived(channelData);
    reply->deleteLater();
}

void YoutubeClient::onRecommendedFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    RecommendedData recommendedData;
    recommendedData.clientVersion = INNERTUBE_CLIENT_VERSION;
    RecommendedPageParser::parse(&recommendedData, &response);
    emit recommendedDataReceived(recommendedData);
    reply->deleteLater();
}

void YoutubeClient::onRecommendedNextBatchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    RecommendedData recommendedData;
    RecommendedPageParser::parseNextBatch(&recommendedData, &response);
    emit recommendedNextBatchReceived(recommendedData);
    reply->deleteLater();
}

void YoutubeClient::onTrendingFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    TrendingData trendingData;
    TrendingPageParser::parse(&trendingData, &response);
    emit trendingDataReceived(trendingData);
    reply->deleteLater();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

QNetworkRequest YoutubeClient::prepareRequest(QString url)
{
    QNetworkRequest request(url);
    // Updated User-Agent — old Firefox 74 UA was getting blocked
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    // Handle CONSENT cookie (YouTube GDPR consent) — updated cookie format
    QNetworkCookieJar *cookieJar = ApplicationUI::networkManager->cookieJar();
    QList<QNetworkCookie> cookies = cookieJar->cookiesForUrl(url);
    bool consentExists = false;

    for (int i = 0; i < cookies.count(); i++) {
        if (QString::fromUtf8(cookies[i].name()) == "CONSENT" ||
            QString::fromUtf8(cookies[i].name()) == "SOCS") {
            consentExists = true;
        }
    }

    if (!consentExists) {
        QList<QNetworkCookie> newCookies;
        // New SOCS cookie replaces old CONSENT cookie since ~2023
        newCookies.append(QNetworkCookie("SOCS", "CAESEwgDEgk0NTk4MjI4NTIaAmVuIAEaBgiA_LysBg"));
        newCookies.append(QNetworkCookie("CONSENT",
            QString("YES+cb.%1-17-p0.en+FX+%2")
            .arg(QDate::currentDate().toString("yyyyMMdd"), "667").toUtf8()));
        cookieJar->setCookiesFromUrl(newCookies, url);
    }

    return request;
}

void YoutubeClient::applyInnerTubeHeaders(QNetworkRequest &request)
{
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0");
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", INNERTUBE_CLIENT_VERSION.toUtf8());
    request.setRawHeader("Origin", "https://www.youtube.com");
    request.setRawHeader("Referer", "https://www.youtube.com/");
}

// QNetworkReply::error() only flags network-level failures (DNS, timeout,
// connection refused, etc). An HTTP 400/403/404/500 response is still a
// "successfully completed" transfer as far as Qt is concerned — the
// error lives in the HTTP status code, which we have to check separately.
// Without this, a request rejected by the server (e.g. missing/invalid API
// key) would silently fall through to JSON parsing of an error payload,
// producing an empty result with no visible error (exactly what happened
// with the blank "Recommended" tab).
bool YoutubeClient::hasHttpError(QNetworkReply *reply, QString *errorMessage)
{
    QVariant statusCodeVariant = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusCodeVariant.isValid()) {
        return false; // no HTTP status available (e.g. pure network failure, handled separately)
    }

    int statusCode = statusCodeVariant.toInt();
    if (statusCode >= 200 && statusCode < 300) {
        return false;
    }

    if (errorMessage) {
        QString reasonPhrase =
            reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        *errorMessage = QString("Server error %1 %2").arg(QString::number(statusCode), reasonPhrase);
    }
    return true;
}

// Extract the base.js URL from a watch page HTML response
QString YoutubeClient::extractBaseJsUrl(const QString &html)
{
    // Modern YouTube embeds the player script URL like:
    // "jsUrl":"/s/player/HASH/player_ias.vflset/en_US/base.js"
    QString key = "\"jsUrl\":\"";
    int idx = html.indexOf(key);
    if (idx >= 0) {
        int start = idx + key.length();
        int end   = html.indexOf('"', start);
        QString path = html.mid(start, end - start);
        if (path.startsWith("/")) {
            return "https://www.youtube.com" + path;
        }
        return path;
    }

    // Fallback: old format src="/s/player/..."
    int scriptIndex = html.indexOf("src=\"/s/player/");
    if (scriptIndex >= 0) {
        int jsUrlStart = scriptIndex + 5;
        int jsUrlEnd   = html.indexOf('"', jsUrlStart);
        return "https://www.youtube.com" + html.mid(jsUrlStart, jsUrlEnd - jsUrlStart);
    }

    return "";
}

// Extract ytInitialData JSON from HTML page (used for metadata/related videos)
QString YoutubeClient::getJson(QString response)
{
    QString json;

    // Modern YouTube uses:  var ytInitialData = {...};
    // The JSON is NOT terminated by "};" reliably (deeply nested). We do a balanced-brace scan.
    // NOTE: brace-init-list ({"a", "b"}) is C++11 syntax and is NOT supported by
    // the BB10 NDK toolchain (GCC 4.6.3 / C++98) — use append() instead.
    QStringList keys;
    keys.append("var ytInitialData = ");
    keys.append("window[\"ytInitialData\"] = ");

    for (int k = 0; k < keys.size(); k++) {
        int startIdx = response.indexOf(keys[k]);
        if (startIdx < 0) continue;

        int jsonStart = startIdx + keys[k].length();
        if (jsonStart >= response.length() || response[jsonStart] != '{') continue;

        // Balanced brace scan
        int depth = 0;
        bool inStr = false;
        bool escape = false;
        int jsonEnd = jsonStart;

        for (int i = jsonStart; i < response.length(); i++) {
            QChar c = response[i];
            if (escape) { escape = false; continue; }
            if (c == '\\' && inStr) { escape = true; continue; }
            if (c == '"') { inStr = !inStr; continue; }
            if (!inStr) {
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) { jsonEnd = i; break; }
                }
            }
        }

        if (jsonEnd > jsonStart) {
            json = response.mid(jsonStart, jsonEnd - jsonStart + 1);
            return json;
        }
    }

    return json;
}

QString YoutubeClient::getApiKey(QString response)
{
    // Extract INNERTUBE_API_KEY from HTML (still present in watch pages)
    QString innertubeKey = "\"INNERTUBE_API_KEY\":\"";
    int i = response.indexOf(innertubeKey);
    if (i < 0) {
        // Fallback key name used in some responses
        innertubeKey = "\"innertubeApiKey\":\"";
        i = response.indexOf(innertubeKey);
    }
    if (i < 0) return "";
    int j = response.indexOf("\"", i + innertubeKey.length());
    return response.mid(i + innertubeKey.length(), j - i - innertubeKey.length());
}
