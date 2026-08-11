#include "BasePage.hpp"

#include "MiniPlayer.hpp"
#include "src/parser/YoutubeClient.hpp"
#include "GlobalPlayerContext.hpp"
#include "src/utils/UIUtils.hpp"
#include "src/parser/YoutubeClient.hpp"
#include "src/applicationui.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/PlayerPage/PlayerPage.hpp"

#include <bb/cascades/Page>
#include <bb/cascades/Container>
#include <bb/cascades/NavigationPane>
#include <bb/cascades/DockLayout>
#include <bb/cascades/Color>
#include <bb/cascades/LabelAutoSizeProperties>
#include <bb/cascades/SystemDefaults>

#include <QDebug>

BasePage::BasePage(bb::cascades::NavigationPane *navigationPane, bool addMiniPlayer) :
        bb::cascades::Page(navigationPane), navigationPane(navigationPane), audioOnly(false), isPlaylist(
                false)
{
    youtubeClient = new YoutubeClient(this);
    invidiousClient = new InvidiousClient(ApplicationUI::invidiousInstanceManager, this);
    playerContext = ApplicationUI::playerContext;
    miniPlayer = addMiniPlayer ? new MiniPlayer(navigationPane) : 0;

    root = new bb::cascades::Container();
    ui = root->ui();
    root->setLayout(new bb::cascades::DockLayout());
    overlay = UIUtils::createOverlay();

    QObject::connect(youtubeClient, SIGNAL(metadataReceived(VideoMetadata, StorageData)), this,
            SLOT(onMetadataReceived(VideoMetadata, StorageData)));
    QObject::connect(youtubeClient, SIGNAL(error(QString)), this, SLOT(onYoutubeError(QString)));

    QObject::connect(invidiousClient, SIGNAL(metadataReceived(VideoMetadata, StorageData)), this,
            SLOT(onMetadataReceived(VideoMetadata, StorageData)));
    QObject::connect(invidiousClient, SIGNAL(error(QString)), this,
            SLOT(onInvidiousError(QString)));
}
void BasePage::onYoutubeError(QString message)
{
    // Reached either directly (search/channel/trending -- calls that
    // never touch Invidious) or as the tail end of the Invidious ->
    // YouTube fallback chain kicked off by onInvidiousError(). Either
    // way, by the time YoutubeClient itself reports an error there's no
    // further fallback left, so just surface it.
    pendingVideoText = "";
    overlay->setVisible(false);
    UIUtils::toastError(message);
}
void BasePage::onInvidiousError(QString message)
{
    Q_UNUSED(message);
    if (pendingVideoText.isEmpty()) {
        // Shouldn't normally happen (InvidiousClient is currently only
        // driven from playVideoFromOutside()/playVideoFromPlaylist(),
        // which always set pendingVideoText first), but guard against it
        // rather than silently dropping the overlay spinner forever.
        overlay->setVisible(false);
        return;
    }

    qDebug() << "[bbtube][invidious] giving up, falling back to direct YouTube client for"
             << pendingVideoText;

    QString text = pendingVideoText;
    youtubeClient->process(text);
}
void BasePage::onMetadataReceived(VideoMetadata videoMetadata, StorageData storageData)
{
    pendingVideoText = "";

    if (storageData.instances.count() == 0) {
        UIUtils::toastError("Source unavailable");
    } else {
        navigationPane->push(
                new PlayerPage(videoMetadata, storageData, this->navigationPane, audioOnly, isPlaylist));
    }

    overlay->setVisible(false);
    audioOnly = false;
    isPlaylist = false;
}
void BasePage::playVideoByIdOrUrl(QString text)
{
    overlay->setVisible(true);

    QString videoId = YoutubeClient::getVideoId(text);
    if (videoId.isEmpty()) {
        // Not a video URL/ID (e.g. a search query) -- Invidious routing
        // in this class is only for direct video playback, so go
        // straight to the normal YoutubeClient path.
        youtubeClient->process(text);
        return;
    }

    pendingVideoText = text;
    invidiousClient->fetchVideo(videoId);
}
void BasePage::playVideoFromOutside(QString url)
{
    isPlaylist = false;
    playVideoByIdOrUrl(url);
}
void BasePage::playVideoFromPlaylist(QString url)
{
    isPlaylist = true;
    playVideoByIdOrUrl(url);
}
void BasePage::setVideoToPlayer()
{
    if (miniPlayer) {
        miniPlayer->setVideo();
    }
}
void BasePage::lazyLoad()
{
}

bb::cascades::Container* BasePage::getTitleContainer()
{
    bb::cascades::Container *headerContainer = new bb::cascades::Container();
    headerContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    headerContainer->setLeftPadding(ui->du(1));
    headerContainer->setTopPadding(ui->du(1));
    headerContainer->setRightPadding(ui->du(1));
    headerContainer->setBottomPadding(ui->du(1));
    headerContainer->setBackground(Color::fromARGB(0xff323232));
    headerContainer->setBottomMargin(ui->du(1));

    title = bb::cascades::Label::create();
    title->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::titleText());
    title->setMultiline(true);
    title->autoSize()->setMaxLineCount(2);
    headerContainer->add(title);

    return headerContainer;
}

void BasePage::setTitle(QString titleText)
{
    title->setText(titleText);
}

void BasePage::setAudioOnly(bool value)
{
    audioOnly = value;
}

void BasePage::setIsPlaylist(bool value)
{
    isPlaylist = value;
}
