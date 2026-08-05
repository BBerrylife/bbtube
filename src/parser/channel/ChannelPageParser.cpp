#include "src/parser/channel/ChannelPageParser.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"

#include <bb/data/JsonDataAccess>
#include <QDebug>

// ChannelPageParser: parses InnerTube /browse responses for a channel's
// "Videos" tab.
//
// KEY FIXES vs. the original (2021) implementation:
//  - YouTube migrated most channel video grids from the legacy
//    "gridRenderer" (with "gridVideoRenderer" items) to the same
//    "richGridRenderer" (with "richItemRenderer" -> "videoRenderer" items)
//    used on the home feed. We now check for richGridRenderer FIRST and
//    fall back to the legacy gridRenderer path for older-style responses.
//  - All list indexing into "contents.toList()[0]" (which would crash if
//    a list was empty) is now bounds-checked.
//  - clientVersion no longer parsed from serviceTrackingParams since the
//    new request flow uses a fixed, known-good INNERTUBE_CLIENT_VERSION
//    (see YoutubeClient.cpp); this field is now just a passthrough.

static QVariantMap firstOrEmpty(const QVariantList &list)
{
    return list.isEmpty() ? QVariantMap() : list[0].toMap();
}

// Simple recursive dumper for debugging unknown JSON shapes, without
// depending on any JsonDataAccess serialization API.
static void dumpVariant(const QVariant &value, const QString &path, int depth)
{
    if (depth > 6) {
        qDebug() << "[bbtube][channelParser] dump" << path << "= <max depth reached>";
        return;
    }

    if (value.type() == QVariant::Map) {
        QVariantMap map = value.toMap();
        qDebug() << "[bbtube][channelParser] dump" << path << "= Map with keys" << map.keys();
        foreach(const QString &key, map.keys()) {
            dumpVariant(map[key], path + "." + key, depth + 1);
        }
    } else if (value.type() == QVariant::List) {
        QVariantList list = value.toList();
        qDebug() << "[bbtube][channelParser] dump" << path << "= List with" << list.count() << "items";
        for (int i = 0; i < list.count() && i < 3; i++) {
            dumpVariant(list[i], path + QString("[%1]").arg(i), depth + 1);
        }
    } else {
        qDebug() << "[bbtube][channelParser] dump" << path << "=" << value.toString();
    }
}

static QVariantList extractVideoItems(const QVariantMap &videosTab, bool *isRichGrid)
{
    qDebug() << "[bbtube][channelParser] extractVideoItems: videosTab keys ="
             << videosTab.keys();
    qDebug() << "[bbtube][channelParser] extractVideoItems: videosTab[content] keys ="
             << videosTab["content"].toMap().keys();

    QVariantMap sectionListRenderer = videosTab["content"].toMap()["sectionListRenderer"].toMap();
    QVariantList sectionContents = sectionListRenderer["contents"].toList();

    QVariantMap firstSection = firstOrEmpty(sectionContents);
    QVariantList itemSectionContents = firstSection["itemSectionRenderer"].toMap()["contents"].toList();
    QVariantMap firstItemSection = firstOrEmpty(itemSectionContents);

    if (firstItemSection.contains("richGridRenderer")) {
        *isRichGrid = true;
        qDebug() << "[bbtube][channelParser] extractVideoItems: matched richGridRenderer (nested)";
        return firstItemSection["richGridRenderer"].toMap()["contents"].toList();
    }

    if (firstItemSection.contains("gridRenderer")) {
        *isRichGrid = false;
        qDebug() << "[bbtube][channelParser] extractVideoItems: matched gridRenderer (nested)";
        return firstItemSection["gridRenderer"].toMap()["items"].toList();
    }

    // Some responses skip the extra itemSectionRenderer wrapper entirely
    if (firstSection.contains("richGridRenderer")) {
        *isRichGrid = true;
        qDebug() << "[bbtube][channelParser] extractVideoItems: matched richGridRenderer (top-level)";
        return firstSection["richGridRenderer"].toMap()["contents"].toList();
    }

    // Some responses skip sectionListRenderer entirely and put richGridRenderer
    // directly under content
    QVariantMap directContent = videosTab["content"].toMap();
    if (directContent.contains("richGridRenderer")) {
        *isRichGrid = true;
        qDebug() << "[bbtube][channelParser] extractVideoItems: matched richGridRenderer (direct under content)";
        return directContent["richGridRenderer"].toMap()["contents"].toList();
    }

    qDebug() << "[bbtube][channelParser] extractVideoItems: NO MATCH."
             << " sectionContents.count() =" << sectionContents.count()
             << " firstSection keys =" << firstSection.keys()
             << " itemSectionContents.count() =" << itemSectionContents.count()
             << " firstItemSection keys =" << firstItemSection.keys();
    *isRichGrid = false;
    return QVariantList();
}

void ChannelPageParser::parse(ChannelPageData *channelData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantMap map = jda.loadFromBuffer(*json).toMap();

    qDebug() << "[bbtube][channelParser] top-level keys =" << map.keys();

    if (map.contains("onResponseReceivedActions")) {
        // handle channel redirect (e.g. custom URL -> canonical channel ID)
        QVariantList actions = map["onResponseReceivedActions"].toList();
        QVariantMap firstAction = firstOrEmpty(actions);
        channelData->redirectChannelId =
                firstAction["navigateAction"].toMap()["endpoint"].toMap()
                ["browseEndpoint"].toMap()["browseId"].toString();

        qDebug() << "[bbtube][channelParser] onResponseReceivedActions branch, redirectChannelId ="
                 << channelData->redirectChannelId;
        return;
    }

    if (!map.contains("contents")) {
        qDebug() << "[bbtube][channelParser] no 'contents' key in response -> bailing out early";
        return;
    }

    // Header moved from "c4TabbedHeaderRenderer" to "pageHeaderRenderer" for
    // many channels (2023+ redesign); try both.
    QVariantMap headerMap = map["header"].toMap()["c4TabbedHeaderRenderer"].toMap();

    if (!headerMap.isEmpty()) {
        channelData->title = headerMap["title"].toString();
        QVariantList avatarThumbs = headerMap["avatar"].toMap()["thumbnails"].toList();
        if (!avatarThumbs.isEmpty()) {
            channelData->thumbnailUrl = avatarThumbs.last().toMap()["url"].toString();
        }
    } else {
        // New "pageHeaderRenderer" shape
        QVariantMap pageHeader = map["header"].toMap()["pageHeaderRenderer"].toMap();
        QVariantMap content = pageHeader["content"].toMap()["pageHeaderViewModel"].toMap();

        QVariantMap titleMap = content["title"].toMap()["dynamicTextViewModel"].toMap()
                ["text"].toMap();
        channelData->title = titleMap.contains("content")
                ? titleMap["content"].toString()
                : pageHeader["pageTitle"].toString();

        QVariantList avatarImages = content["image"].toMap()["decoratedAvatarViewModel"].toMap()
                ["avatar"].toMap()["avatarViewModel"].toMap()["image"].toMap()
                ["sources"].toList();
        if (!avatarImages.isEmpty()) {
            channelData->thumbnailUrl = avatarImages.last().toMap()["url"].toString();
        }

        if (channelData->title.isEmpty()) {
            channelData->title = pageHeader["pageTitle"].toString();
        }
    }

    qDebug() << "[bbtube][channelParser] parsed title =" << channelData->title
             << ", thumbnailUrl empty =" << channelData->thumbnailUrl.isEmpty();

    QVariantList tabList =
            map["contents"].toMap()["twoColumnBrowseResultsRenderer"].toMap()["tabs"].toList();
    QVariantMap videosTab;

    qDebug() << "[bbtube][channelParser] tabList.count() =" << tabList.count();

    for (int i = 0; i < tabList.count(); i++) {
        QVariantMap tab = tabList[i].toMap()["tabRenderer"].toMap();
        QVariantMap tabEndpoint = tab["endpoint"].toMap()["browseEndpoint"].toMap();

        qDebug() << "[bbtube][channelParser]  tab[" << i << "] title ="
                 << tab["title"].toString()
                 << ", canonicalBaseUrl =" << tabEndpoint["canonicalBaseUrl"].toString()
                 << ", hasContent =" << tab.contains("content")
                 << ", selected =" << tab["selected"].toBool();

        if (tab.contains("content")) {
            videosTab = tab;
        }
    }

    if (videosTab.isEmpty()) {
        // No tab with content found (e.g. channel has no videos, or the
        // "Videos" tab itself wasn't returned) — leave videos empty.
        qDebug() << "[bbtube][channelParser] no tab with 'content' found -> videos will be empty";
        return;
    }

    bool isRichGrid = false;
    QVariantList videosList = extractVideoItems(videosTab, &isRichGrid);

    qDebug() << "[bbtube][channelParser] videosList.count() =" << videosList.count()
             << ", isRichGrid =" << isRichGrid;

    int matchedCount = 0;
    int noVideoIdCount = 0;
    int unmatchedCount = 0;

    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (i < 3) {
            qDebug() << "[bbtube][channelParser] videosList[" << i << "] keys =" << item.keys();
        }

        if (isRichGrid && item.contains("richItemRenderer")) {
            QVariantMap contentMap = item["richItemRenderer"].toMap()["content"].toMap();
            QVariantMap videoMap = contentMap["videoRenderer"].toMap();

            if (videoMap.contains("videoId")) {
                SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
                video.channelId = channelData->channelId;
                video.channelTitle = channelData->title;
                channelData->videos.append(video);
                matchedCount++;
            } else if (contentMap.contains("lockupViewModel")) {
                QVariantMap lockupViewModel = contentMap["lockupViewModel"].toMap();
                SingleVideoMetadata video = ItemRendererParser::getVideoFromLockup(lockupViewModel);

                if (video.videoId.isEmpty()) {
                    noVideoIdCount++;
                    continue;
                }

                video.channelId = channelData->channelId;
                video.channelTitle = channelData->title;
                channelData->videos.append(video);
                matchedCount++;
            } else {
                noVideoIdCount++;
            }
        } else if (!isRichGrid && item.contains("gridVideoRenderer")) {
            QVariantMap videoMap = item["gridVideoRenderer"].toMap();
            SingleVideoMetadata video = ItemRendererParser::getFromChannelVideo(&videoMap);

            video.channelId = channelData->channelId;
            video.channelTitle = channelData->title;
            channelData->videos.append(video);
            matchedCount++;
        } else if (item.contains("continuationItemRenderer")) {
            channelData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        } else {
            unmatchedCount++;
        }
    }

    qDebug() << "[bbtube][channelParser] loop finished: matchedCount =" << matchedCount
             << ", noVideoIdCount =" << noVideoIdCount
             << ", unmatchedCount =" << unmatchedCount
             << ", channelData->videos.count() =" << channelData->videos.count();
}

void ChannelPageParser::parseNextBatch(ChannelPageData *channelData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantList receivedActions =
            jda.loadFromBuffer(*json).toMap()["onResponseReceivedActions"].toList();

    if (receivedActions.count() == 0) {
        return;
    }

    QVariantMap firstAction = receivedActions[0].toMap();
    QVariantList videosList =
            firstAction["appendContinuationItemsAction"].toMap()["continuationItems"].toList();

    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (item.contains("richItemRenderer")) {
            QVariantMap contentMap = item["richItemRenderer"].toMap()["content"].toMap();
            QVariantMap videoMap = contentMap["videoRenderer"].toMap();

            if (videoMap.contains("videoId")) {
                SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
                video.channelId = channelData->channelId;
                video.channelTitle = channelData->title;
                channelData->videos.append(video);
            } else if (contentMap.contains("lockupViewModel")) {
                QVariantMap lockupViewModel = contentMap["lockupViewModel"].toMap();
                SingleVideoMetadata video = ItemRendererParser::getVideoFromLockup(lockupViewModel);

                if (video.videoId.isEmpty()) {
                    continue;
                }

                video.channelId = channelData->channelId;
                video.channelTitle = channelData->title;
                channelData->videos.append(video);
            }
        } else if (item.contains("gridVideoRenderer")) {
            QVariantMap videoMap = item["gridVideoRenderer"].toMap();
            SingleVideoMetadata video = ItemRendererParser::getFromChannelVideo(&videoMap);

            video.channelId = channelData->channelId;
            video.channelTitle = channelData->title;
            channelData->videos.append(video);
        } else if (item.contains("continuationItemRenderer")) {
            channelData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        }
    }
}
