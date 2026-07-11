#include "TrendingPageParser.hpp"
#include "src/parser/models/TrendingData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"

#include <bb/data/JsonDataAccess>

// TrendingPageParser: parses InnerTube /browse?browseId=FEtrending responses.
//
// KEY FIXES vs. the original (2021) implementation:
//  - All "toList()[0]" indexing into possibly-empty lists is now
//    bounds-checked instead of crashing on out-of-bounds access.
//  - YouTube's Trending page layout changed: the flat
//    "expandedShelfContentsRenderer" shelf is now frequently replaced by a
//    "richGridRenderer" (same shape as the home feed) for the "Now" /
//    default trending tab. We detect and handle both shapes.
//  - Category sub-menu (Music/Gaming/Movies/News tabs) renderer key names
//    are unchanged, but are now guarded against being absent entirely
//    (some locales no longer return a subMenu).

static QVariantMap firstOrEmpty(const QVariantList &list)
{
    return list.isEmpty() ? QVariantMap() : list[0].toMap();
}

void TrendingPageParser::parse(TrendingData *trendingData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariantMap map = jda.loadFromBuffer(*json).toMap();

    QVariantList tabs =
            map["contents"].toMap()["twoColumnBrowseResultsRenderer"].toMap()["tabs"].toList();
    QVariantMap firstTab = firstOrEmpty(tabs);
    QVariantMap sectionListRendererMap =
            firstTab["tabRenderer"].toMap()["content"].toMap()["sectionListRenderer"].toMap();

    QVariantList contentsList = sectionListRendererMap["contents"].toList();

    for (int i = 0; i < contentsList.count(); i++) {
        QVariantMap section = contentsList[i].toMap();

        // Modern shape: richGridRenderer directly inside an itemSectionRenderer
        QVariantList itemSectionContents =
                section["itemSectionRenderer"].toMap()["contents"].toList();
        QVariantMap firstItemSectionContent = firstOrEmpty(itemSectionContents);

        if (firstItemSectionContent.contains("richGridRenderer")) {
            QVariantList richItems =
                    firstItemSectionContent["richGridRenderer"].toMap()["contents"].toList();

            for (int j = 0; j < richItems.count(); j++) {
                QVariantMap richItem = richItems[j].toMap();
                if (!richItem.contains("richItemRenderer")) {
                    continue;
                }

                QVariantMap videoMap = richItem["richItemRenderer"].toMap()["content"].toMap()
                        ["videoRenderer"].toMap();
                if (!videoMap.contains("videoId")) {
                    continue;
                }

                trendingData->videos.append(ItemRendererParser::getVideo(&videoMap));
            }

            continue;
        }

        // Legacy shape: shelfRenderer -> expandedShelfContentsRenderer -> items
        if (!firstItemSectionContent.contains("shelfRenderer")) {
            continue;
        }

        QVariantList videoList = firstItemSectionContent["shelfRenderer"].toMap()["content"].toMap()
                ["expandedShelfContentsRenderer"].toMap()["items"].toList();

        for (int j = 0; j < videoList.count(); j++) {
            QVariantMap videoMap = videoList[j].toMap()["videoRenderer"].toMap();
            if (!videoMap.contains("videoId")) {
                continue;
            }

            trendingData->videos.append(ItemRendererParser::getVideo(&videoMap));
        }
    }

    if (sectionListRendererMap.contains("subMenu")) {
        QVariantList categoryList =
                sectionListRendererMap["subMenu"].toMap()["channelListSubMenuRenderer"].toMap()
                ["contents"].toList();

        for (int i = 0; i < categoryList.count(); i++) {
            QVariantMap categoryMap =
                    categoryList[i].toMap()["channelListSubMenuAvatarRenderer"].toMap();
            if (categoryMap.isEmpty()) {
                continue;
            }

            TrendingDataCategory category;
            category.title = categoryMap["title"].toMap()["simpleText"].toString();
            category.categoryKey =
                    categoryMap["navigationEndpoint"].toMap()["browseEndpoint"].toMap()
                    ["params"].toString();

            trendingData->categories.append(category);
        }
    }
}
