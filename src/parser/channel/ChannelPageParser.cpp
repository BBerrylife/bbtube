#include "src/parser/channel/ChannelPageParser.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"

#include <bb/data/JsonDataAccess>

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

// Walk a tab's content, looking for a video grid in either the modern
// (richGridRenderer) or legacy (gridRenderer) shape. Returns the list of
// renderer items ("richItemRenderer" / "gridVideoRenderer" / "continuationItemRenderer")
// and sets isRichGrid accordingly.
static QVariantList extractVideoItems(const QVariantMap &videosTab, bool *isRichGrid)
{
    QVariantMap sectionListRenderer = videosTab["content"].toMap()["sectionListRenderer"].toMap();
    QVariantList sectionContents = sectionListRenderer["contents"].toList();

    QVariantMap firstSection = firstOrEmpty(sectionContents);
    QVariantList itemSectionContents = firstSection["itemSectionRenderer"].toMap()["contents"].toList();
    QVariantMap firstItemSection = firstOrEmpty(itemSectionContents);

    if (firstItemSection.contains("richGridRenderer")) {
        *isRichGrid = true;
        return firstItemSection["richGridRenderer"].toMap()["contents"].toList();
    }

    if (firstItemSection.contains("gridRenderer")) {
        *isRichGrid = false;
        return firstItemSection["gridRenderer"].toMap()["items"].toList();
    }

    // Some responses skip the extra itemSectionRenderer wrapper entirely
    if (firstSection.contains("richGridRenderer")) {
        *isRichGrid = true;
        return firstSection["richGridRenderer"].toMap()["contents"].toList();
    }

    *isRichGrid = false;
    return QVariantList();
}

void ChannelPageParser::parse(ChannelPageData *channelData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantMap map = jda.loadFromBuffer(*json).toMap();

    if (map.contains("onResponseReceivedActions")) {
        // handle channel redirect (e.g. custom URL -> canonical channel ID)
        QVariantList actions = map["onResponseReceivedActions"].toList();
        QVariantMap firstAction = firstOrEmpty(actions);
        channelData->redirectChannelId =
                firstAction["navigateAction"].toMap()["endpoint"].toMap()
                ["browseEndpoint"].toMap()["browseId"].toString();

        return;
    }

    if (!map.contains("contents")) {
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

    QVariantList tabList =
            map["contents"].toMap()["twoColumnBrowseResultsRenderer"].toMap()["tabs"].toList();
    QVariantMap videosTab;

    for (int i = 0; i < tabList.count(); i++) {
        QVariantMap tab = tabList[i].toMap()["tabRenderer"].toMap();

        if (tab.contains("content")) {
            videosTab = tab;
        }
    }

    if (videosTab.isEmpty()) {
        // No tab with content found (e.g. channel has no videos, or the
        // "Videos" tab itself wasn't returned) — leave videos empty.
        return;
    }

    bool isRichGrid = false;
    QVariantList videosList = extractVideoItems(videosTab, &isRichGrid);

    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (isRichGrid && item.contains("richItemRenderer")) {
            QVariantMap videoMap =
                    item["richItemRenderer"].toMap()["content"].toMap()["videoRenderer"].toMap();
            if (!videoMap.contains("videoId")) {
                continue;
            }

            SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
            video.channelId = channelData->channelId;
            video.channelTitle = channelData->title;
            channelData->videos.append(video);
        } else if (!isRichGrid && item.contains("gridVideoRenderer")) {
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
            QVariantMap videoMap =
                    item["richItemRenderer"].toMap()["content"].toMap()["videoRenderer"].toMap();
            if (!videoMap.contains("videoId")) {
                continue;
            }

            SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
            video.channelId = channelData->channelId;
            video.channelTitle = channelData->title;
            channelData->videos.append(video);
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
