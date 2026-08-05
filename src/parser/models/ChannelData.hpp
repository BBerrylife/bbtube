#ifndef ChannelData_HPP_
#define ChannelData_HPP_

#include "VideoMetadata.hpp"

#include <QMetaType>

class ChannelData
{
public:
    QString channelId;
    QString title;
    QString thumbnailUrl;
    QString redirectChannelId;
};

class ChannelPageData : public ChannelData
{
public:
    QString clientVersion;
    QList<SingleVideoMetadata> videos;
    QString ctoken;
    QString apiKey;
};

Q_DECLARE_METATYPE(ChannelPageData)

#endif /* ChannelData_HPP_ */
