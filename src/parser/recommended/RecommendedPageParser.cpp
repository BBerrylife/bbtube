#include "RecommendedPageParser.hpp"
#include "src/parser/models/RecommendedData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"

#include <bb/data/JsonDataAccess>
#include <QDebug>

// Simple recursive dumper for debugging unknown JSON shapes, without
// depending on any JsonDataAccess serialization API.
static void dumpVariant(const QVariant &value, const QString &path, int depth)
{
    if (depth > 9) {
        qDebug() << "[bbtube][recommendedParser] dump" << path << "= <max depth reached>";
        return;
    }

    if (value.type() == QVariant::Map) {
        QVariantMap map = value.toMap();
        qDebug() << "[bbtube][recommendedParser] dump" << path << "= Map with keys" << map.keys();
        foreach(const QString &key, map.keys()) {
            dumpVariant(map[key], path + "." + key, depth + 1);
        }
    } else if (value.type() == QVariant::List) {
        QVariantList list = value.toList();
        qDebug() << "[bbtube][recommendedParser] dump" << path << "= List with" << list.count() << "items";
        for (int i = 0; i < list.count() && i < 2; i++) {
            dumpVariant(list[i], path + QString("[%1]").arg(i), depth + 1);
        }
    } else {
        qDebug() << "[bbtube][recommendedParser] dump" << path << "=" << value.toString();
    }
}

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

    qDebug() << "[bbtube][recommendedParser] top-level keys =" << map.keys();
    qDebug() << "[bbtube][recommendedParser] gridRendererMap keys =" << gridRendererMap.keys();
    qDebug() << "[bbtube][recommendedParser] videosList.count() =" << videosList.count();

    // As of the 2025+ homepage redesign, the grid's top-level "contents" is
    // often a single "richSectionRenderer" wrapper (a shelf/section), with
    // the actual richItemRenderer video items nested one level deeper
    // inside it (richShelfRenderer.contents). Flatten that here so the main
    // loop below can keep working on a flat list of richItemRenderer items.
    QVariantList flatVideosList;
    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (item.contains("richSectionRenderer")) {
            QVariantMap sectionContent =
                    item["richSectionRenderer"].toMap()["content"].toMap();

            QVariantList shelfItems;
            if (sectionContent.contains("richShelfRenderer")) {
                shelfItems = sectionContent["richShelfRenderer"].toMap()["contents"].toList();
            } else if (sectionContent.contains("richGridRenderer")) {
                shelfItems = sectionContent["richGridRenderer"].toMap()["contents"].toList();
            }

            if (i == 0) {
                qDebug() << "[bbtube][recommendedParser] richSectionRenderer.content keys ="
                         << sectionContent.keys() << ", shelfItems.count() =" << shelfItems.count();
            }

            for (int j = 0; j < shelfItems.count(); j++) {
                flatVideosList.append(shelfItems[j]);
            }
        } else {
            flatVideosList.append(item);
        }
    }

    if (!flatVideosList.isEmpty()) {
        videosList = flatVideosList;
        qDebug() << "[bbtube][recommendedParser] after flattening richSectionRenderer, "
                    "videosList.count() =" << videosList.count();
    }

    int matchedCount = 0;
    int noVideoIdCount = 0;
    int unmatchedCount = 0;

    for (int i = 0; i < videosList.count(); i++) {
        QVariantMap item = videosList[i].toMap();

        if (i < 3) {
            qDebug() << "[bbtube][recommendedParser] videosList[" << i << "] keys =" << item.keys();
        }

        if (item.contains("richItemRenderer")) {
            QVariantMap contentMap = item["richItemRenderer"].toMap()["content"].toMap();
            QVariantMap videoMap = contentMap["videoRenderer"].toMap();

            if (i < 3) {
                qDebug() << "[bbtube][recommendedParser] videosList[" << i
                         << "] richItemRenderer.content keys =" << contentMap.keys();
            }

            if (videoMap.contains("videoId")) {
                SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
                recommendedData->videos.append(video);
                matchedCount++;
            } else if (contentMap.contains("lockupViewModel")) {
                SingleVideoMetadata video =
                        ItemRendererParser::getVideoFromLockup(contentMap["lockupViewModel"].toMap());

                if (video.videoId.isEmpty()) {
                    noVideoIdCount++;
                    continue;
                }

                recommendedData->videos.append(video);
                matchedCount++;
            } else {
                noVideoIdCount++;
            }
        } else if (item.contains("continuationItemRenderer")) {
            recommendedData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        } else {
            unmatchedCount++;
            if (i == 0) {
                dumpVariant(videosList[i], QString("videosList[%1]").arg(i), 0);
            }
        }
    }

    qDebug() << "[bbtube][recommendedParser] loop finished: matchedCount =" << matchedCount
             << ", noVideoIdCount =" << noVideoIdCount
             << ", unmatchedCount =" << unmatchedCount
             << ", recommendedData->videos.count() =" << recommendedData->videos.count();
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
            QVariantMap contentMap = item["richItemRenderer"].toMap()["content"].toMap();
            QVariantMap videoMap = contentMap["videoRenderer"].toMap();

            if (videoMap.contains("videoId")) {
                SingleVideoMetadata video = ItemRendererParser::getVideo(&videoMap);
                recommendedData->videos.append(video);
            } else if (contentMap.contains("lockupViewModel")) {
                SingleVideoMetadata video =
                        ItemRendererParser::getVideoFromLockup(contentMap["lockupViewModel"].toMap());

                if (video.videoId.isEmpty()) {
                    continue;
                }

                recommendedData->videos.append(video);
            }
        } else if (item.contains("continuationItemRenderer")) {
            recommendedData->ctoken =
                    item["continuationItemRenderer"].toMap()["continuationEndpoint"].toMap()
                    ["continuationCommand"].toMap()["token"].toString();
        }
    }
}
