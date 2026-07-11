#include "RecommendedPageParser.hpp"
#include "src/parser/models/RecommendedData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"

#include <bb/data/JsonDataAccess>

// RecommendedPageParser: parses InnerTube /browse?browseId=FEwhat_to_watch
// (the home/"recommended" feed) responses.
//
// KEY FIXES vs. the original (2021) implementation:
//  - "tabs.toList()[0]" is now bounds-checked (an empty tabs list used to
//    crash via out-of-bounds access).
//  - richGridRenderer is sometimes nested one level deeper inside a
//    sectionListRenderer wrapper on certain account/locale variants; we now
//    check both shapes.
//  - clientVersion parsing kept as a best-effort fallback, but
//    YoutubeClient.cpp no longer depends on it being correct since it now
//    uses a fixed known-good version for continuation requests.

static QVariantMap firstOrEmpty(const QVariantList &list)
{
    return list.isEmpty() ? QVariantMap() : list[0].toMap();
}

static QVariantMap extractRichGridRenderer(const QVariantMap &map)
{
    QVariantList tabs =
            map["contents"].toMap()["twoColumnBrowseResultsRenderer"].toMap()["tabs"].toList();
    QVariantMap firstTab = firstOrEmpty(tabs);
    QVariantMap tabContent = firstTab["tabRenderer"].toMap()["content"].toMap();

    QVariantMap richGrid = tabContent["richGridRenderer"].toMap();
    if (!richGrid.isEmpty()) {
        return richGrid;
    }

    // Fallback: richGridRenderer nested inside a sectionListRenderer
    QVariantList sectionContents = tabContent["sectionListRenderer"].toMap()["contents"].toList();
    QVariantMap firstSection = firstOrEmpty(sectionContents);
    QVariantList itemSectionContents = firstSection["itemSectionRenderer"].toMap()["contents"].toList();
    QVariantMap firstItemSection = firstOrEmpty(itemSectionContents);

    return firstItemSection["richGridRenderer"].toMap();
}

void RecommendedPageParser::parse(RecommendedData *recommendedData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantMap map = jda.loadFromBuffer(*json).toMap();

    QVariantList serviceTrackingParams =
            map["responseContext"].toMap()["serviceTrackingParams"].toList();

    for (int i = 0; i < serviceTrackingParams.count(); i++) {
        QVariantMap map1 = serviceTrackingParams[i].toMap();
        if (map1["service"].toString() != "CSI") {
            continue;
        }

        QVariantList paramsList = map1["params"].toList();

        for (int j = 0; j < paramsList.count(); j++) {
            QVariantMap map2 = paramsList[j].toMap();

            if (map2["key"].toString() == "cver") {
                recommendedData->clientVersion = map2["value"].toString();
                break;
            }
        }

        if (recommendedData->clientVersion != "") {
            break;
        }
    }

    QVariantMap gridRendererMap = extractRichGridRenderer(map);
    QVariantList videosList = gridRendererMap["contents"].toList();

    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (item.contains("richItemRenderer")) {
            QVariantMap videoMap =
                    item["richItemRenderer"].toMap()["content"].toMap()["videoRenderer"].toMap();
            if (!videoMap.contains("videoId")) {
                continue;
            }

            SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
            recommendedData->videos.append(video);
        } else if (item.contains("continuationItemRenderer")) {
            recommendedData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        }
    }
}

void RecommendedPageParser::parseNextBatch(RecommendedData *recommendedData, QString *json)
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
            recommendedData->videos.append(video);
        } else if (item.contains("continuationItemRenderer")) {
            recommendedData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        }
    }
}
