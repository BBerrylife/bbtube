#ifndef RECOMMENDEDDATA_HPP_
#define RECOMMENDEDDATA_HPP_

#include "VideoMetadata.hpp"

class RecommendedData
{
public:
    QString clientVersion;
    QList<SingleVideoMetadata> videos;
    QString ctoken;
    QString apiKey;
    // Set when YouTube returns a "feed nudge" instead of actual video
    // recommendations (e.g. for a fresh/anonymous session with no watch
    // history to personalize from). Empty when real videos were returned.
    QString emptyFeedMessage;
};

#endif /* RECOMMENDEDDATA_HPP_ */
