#include "TrendingPageParser.hpp"
#include "src/parser/models/TrendingData.hpp"

#include <bb/data/JsonDataAccess>
#include <QDebug>

// TrendingPageParser: parses YouTube Charts (charts.youtube.com) responses.
//
// As of July 2025, YouTube shut down the classic Trending page (browseId
// FEtrending) entirely, replacing it with category-specific "Charts". The
// response shape is completely different from the old
// twoColumnBrowseResultsRenderer-based Trending page: it's a single
// "musicAnalyticsSectionRenderer" section whose content.videos[0].videoViews
// is the flat array of chart entries (see chart_params_chart_type in
// YoutubeClient::trending() for the available chart types). Each entry has
// its own id/title/thumbnail/duration fields (not a videoRenderer), so we
// map them directly here instead of going through ItemRendererParser.

static QVariantMap firstOrEmpty(const QVariantList &list)
{
    return list.isEmpty() ? QVariantMap() : list[0].toMap();
}

// Pick the largest available thumbnail from a Charts "thumbnails" array
// (they come pre-sorted smallest-to-largest, same as elsewhere in the app).
static QString largestThumbnailUrl(const QVariantList &thumbnails)
{
    if (thumbnails.isEmpty()) {
        return QString();
    }
    return thumbnails.last().toMap()["url"].toString();
}

// Charts gives duration as a plain seconds string ("411"); the rest of the
// app expects a formatted "m:ss" / "h:mm:ss" string (SingleVideoMetadata's
// isLiveStream() also treats an empty lengthText as "this is a livestream",
// so we deliberately leave it empty if duration is missing/zero).
static QString formatDurationSeconds(const QString &secondsStr)
{
    bool ok = false;
    int totalSeconds = secondsStr.toInt(&ok);
    if (!ok || totalSeconds <= 0) {
        return QString();
    }

    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    if (hours > 0) {
        return QString("%1:%2:%3").arg(hours)
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

void TrendingPageParser::parse(TrendingData *trendingData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantMap map = jda.loadFromBuffer(*json).toMap();

    QVariantList sectionListContents =
            map["contents"].toMap()["sectionListRenderer"].toMap()["contents"].toList();
    QVariantMap firstSection = firstOrEmpty(sectionListContents);
    QVariantMap musicAnalyticsContent =
            firstSection["musicAnalyticsSectionRenderer"].toMap()["content"].toMap();

    QVariantList chartVideosWrapper = musicAnalyticsContent["videos"].toList();
    QVariantMap firstChartVideos = firstOrEmpty(chartVideosWrapper);
    QVariantList entries = firstChartVideos["videoViews"].toList();

    qDebug() << "[bbtube][trendingParser] entries.count() =" << entries.count();

    for (int i = 0; i < entries.count(); i++) {
        QVariantMap entry = entries[i].toMap();

        QString videoId = entry["id"].toString();
        if (videoId.isEmpty()) {
            continue;
        }

        SingleVideoMetadata video;
        video.videoId = videoId;
        video.title = entry["title"].toString();
        video.channelTitle = entry["channelName"].toString();
        video.channelId = entry["externalChannelId"].toString();
        video.thumbnailUrl =
                largestThumbnailUrl(entry["thumbnail"].toMap()["thumbnails"].toList());
        video.lengthText = formatDurationSeconds(entry["videoDuration"].toString());

        QString position = entry["chartEntryMetadata"].toMap()["currentPosition"].toString();
        if (!position.isEmpty()) {
            video.shortViewsCount = "#" + position;
        }

        trendingData->videos.append(video);
    }

    qDebug() << "[bbtube][trendingParser] parsed videos.count() =" << trendingData->videos.count();
}
