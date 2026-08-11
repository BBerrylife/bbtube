#ifndef BASEPAGE_HPP_
#define BASEPAGE_HPP_

#include "MiniPlayer.hpp"
#include "src/parser/YoutubeClient.hpp"
#include "GlobalPlayerContext.hpp"
#include "src/parser/YoutubeClient.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/invidious/InvidiousClient.hpp"

#include <bb/cascades/Page>
#include <bb/cascades/Container>
#include <bb/cascades/NavigationPane>
#include <bb/cascades/UIConfig>

class BasePage: public bb::cascades::Page
{
Q_OBJECT
private:
    bb::cascades::Label *title;
protected:
    YoutubeClient *youtubeClient;
    InvidiousClient *invidiousClient;
    GlobalPlayerContext *playerContext;
    MiniPlayer *miniPlayer;

    bb::cascades::NavigationPane *navigationPane;
    bb::cascades::Container *root;
    bb::cascades::Container *overlay;
    bb::cascades::UIConfig *ui;

    bool audioOnly;
    bool isPlaylist;
    // Set while a playVideoFromOutside()/playVideoFromPlaylist() call is
    // trying InvidiousClient first; lets onYoutubeError() know a
    // YoutubeClient error during this window is unrelated noise to
    // ignore (see comment in the .cpp), and tells onInvidiousError() to
    // fall back to youtubeClient->process() rather than surfacing the
    // error directly.
    QString pendingVideoText;
protected slots:
    virtual void onYoutubeError(QString message);
    virtual void onMetadataReceived(VideoMetadata videoMetadata, StorageData storageData);
    virtual void onInvidiousError(QString message);
protected:
    // Shared Invidious-first / YoutubeClient-fallback logic, used by
    // playVideoFromOutside()/playVideoFromPlaylist() below AND by
    // subclasses (see PlayerPage::playVideoFromOutside/FromPlaylist) that
    // override those two virtuals to add their own pre/post steps (e.g.
    // pausing the current player) but still want the same Invidious
    // routing rather than calling youtubeClient->process() directly.
    // `text` is anything YoutubeClient::getVideoId() accepts: a full
    // watch URL, bare video ID, or search text (falls back to
    // youtubeClient->process() immediately for non-video text).
    void playVideoByIdOrUrl(QString text);
public:
    BasePage(bb::cascades::NavigationPane *navigationPane, bool addMiniPlayer = true);
    virtual ~BasePage()
    {
    }
    virtual void playVideoFromOutside(QString url);
    virtual void playVideoFromPlaylist(QString url);
    virtual void setVideoToPlayer();
    virtual void lazyLoad();

    bb::cascades::Container *getTitleContainer();
    void setTitle(QString title);
    void setAudioOnly(bool value);
    void setIsPlaylist(bool value);
};

#endif /* BASEPAGE_HPP_ */
