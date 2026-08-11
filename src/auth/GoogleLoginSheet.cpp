#include "GoogleLoginSheet.hpp"
#include "GoogleAuthManager.hpp"
#include "src/applicationui.hpp"

#include <bb/cascades/Page>
#include <bb/cascades/TitleBar>
#include <bb/cascades/ActionItem>
#include <bb/cascades/WebStorage>
#include <bb/cascades/WebCookieJar>
#include <bb/cascades/WebSettings>

GoogleLoginSheet::GoogleLoginSheet() :
        BaseSheet(), webView(0), captured(false)
{
    Page *content = new Page();

    webView = WebView::create();
    webView->setUrl(QUrl("https://accounts.google.com/ServiceLogin?"
            "continue=https://myaccount.google.com"));
    // Cookies are on by default, but be explicit -- the whole point of this
    // sheet is to capture the session cookies afterward.
    webView->settings()->setCookiesEnabled(true);

    QObject::connect(webView, SIGNAL(urlChanged(QUrl)), this, SLOT(onUrlChanged(QUrl)));

    TitleBar *titleBar = new TitleBar(TitleBarKind::Default);
    ActionItem *cancelAction = ActionItem::create().title("Cancel");
    QObject::connect(cancelAction, SIGNAL(triggered()), this, SLOT(closeActionClick()));
    titleBar->setTitle("Sign in to Google");
    titleBar->setDismissAction(cancelAction);

    content->setTitleBar(titleBar);
    content->setContent(webView);
    this->setContent(content);

    open();
}

void GoogleLoginSheet::onUrlChanged(const QUrl &url)
{
    if (captured) {
        return;
    }

    // myaccount.google.com is only reached after a *completed* sign-in --
    // intermediate steps (password entry, 2FA challenge, "verify it's you"
    // prompts) all stay on accounts.google.com. This is a heuristic, not
    // an official signal; see header comment.
    if (url.host() == "myaccount.google.com") {
        captureSessionAndClose();
    }
}

void GoogleLoginSheet::captureSessionAndClose()
{
    captured = true;

    bb::cascades::WebCookieJar *jar = webView->storage()->cookieJar();
    QStringList cookies = jar->cookiesForUrl(QUrl("https://www.google.com"));

    // Email label is left blank for now -- Settings will show a generic
    // "Logged in" state rather than "Logged in as name@gmail.com". Scraping
    // the email out of the post-login page via evaluateJavaScript() is
    // possible as a follow-up (requires wiring up the
    // javaScriptResult(int,QVariant) signal to correlate the async result
    // with this call, which didn't fit in this pass) but isn't needed for
    // login/cookie-capture to work.
    if (cookies.isEmpty()) {
        // Landed on myaccount.google.com but the cookie jar came back
        // empty -- something unexpected happened (e.g. domain mismatch).
        // Don't save a broken session; let the user retry.
        closeSheet();
        return;
    }

    ApplicationUI::googleAuthManager->saveSession(cookies, "");

    closeSheet();
}
