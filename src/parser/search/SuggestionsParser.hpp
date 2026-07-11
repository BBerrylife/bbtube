#ifndef SuggestionsParser_HPP_
#define SuggestionsParser_HPP_

#include <bb/data/JsonDataAccess>
#include <QStringList>

// SuggestionsParser: parses the YouTube search-suggestions response.
//
// KEY FIX vs. the original (2021) implementation:
//  - outer[1] was previously accessed without checking the outer list's
//    size, which crashes if the suggestions endpoint returns an empty,
//    malformed, or differently-shaped payload (this happens more often now
//    since the endpoint isn't officially documented and occasionally
//    changes shape slightly between regions). We now bounds-check every
//    level before indexing.
class SuggestionsParser
{
public:
    static QStringList parseSuggestions(const QString *json)
    {
        QStringList result;

        bb::data::JsonDataAccess jda;
        QVariantList outer = jda.loadFromBuffer(*json).toList();

        if (outer.size() < 2) {
            return result;
        }

        QVariantList list = outer[1].toList();

        for (int i = 0; i < list.size(); i++) {
            QVariantList entry = list[i].toList();
            if (entry.isEmpty()) {
                continue;
            }
            result.append(entry[0].toString());
        }

        return result;
    }
};

#endif /* SuggestionsParser_HPP_ */
