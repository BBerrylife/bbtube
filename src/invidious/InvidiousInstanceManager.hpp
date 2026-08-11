#ifndef INVIDIOUSINSTANCEMANAGER_HPP_
#define INVIDIOUSINSTANCEMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

// Maintains bbtube's pool of Invidious instance base URLs (e.g.
// "https://inv.nadeko.net"), modeled on notPipe's (gohoski/notPipe)
// multi-instance strategy: rather than hardcoding one instance -- which
// dies the moment it gets enough traffic for YouTube to notice and block
// it -- we fetch the community-maintained list from api.invidious.io,
// which only lists instances meeting uptime/maintenance requirements
// (see https://docs.invidious.io/instances/), and pick a new random
// instance for each request. This spreads load and avoids a single point
// of failure.
//
// IMPORTANT CAVEAT (do not remove this comment without re-reading it):
// Invidious instances are not immune to the same PO-Token/SABR problems
// bbtube's direct InnerTube client hits -- see the large comment above
// INNERTUBE_PLAYER_CLIENTS in YoutubeClient.cpp for the full background.
// Some instances run "invidious-companion" (a server-side BotGuard
// solver) and have a much better success rate than bbtube calling
// YouTube directly, but there is no guarantee any given instance, at any
// given moment, can actually produce a playable URL for a given video.
// InvidiousClient (separate file) is expected to try more than one
// instance per video if the first one comes back empty.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class InvidiousInstanceManager: public QObject
{
Q_OBJECT
public:
    explicit InvidiousInstanceManager(QObject *parent = 0);
    virtual ~InvidiousInstanceManager()
    {
    }

    // Kicks off a background refresh of the instance list from
    // api.invidious.io. Safe to call multiple times; a refresh already in
    // flight is not duplicated. Not required before calling
    // pickRandomInstance() -- see below.
    void refreshInstanceList();

    // Returns a randomly chosen instance base URL with no trailing slash
    // (e.g. "https://inv.nadeko.net"), or "" if no instance list is
    // available yet (refreshInstanceList() hasn't completed even once,
    // and the hardcoded fallback below is somehow also empty -- should
    // not happen in practice).
    //
    // Before the first successful refreshInstanceList() completes (e.g.
    // app's first-ever launch, or launched offline), this picks from a
    // small hardcoded fallback list of long-standing, well-known public
    // instances (see FALLBACK_INSTANCES in the .cpp) rather than
    // returning "" -- so video playback isn't blocked on this being the
    // very first network call the app happens to make.
    QString pickRandomInstance() const;

    // Returns true once at least one successful refresh has completed
    // (i.e. pickRandomInstance() is drawing from the live, uptime-vetted
    // list rather than the hardcoded fallback).
    bool hasLiveList() const
    {
        return !liveInstances.isEmpty();
    }

signals:
    // Emitted after a refresh attempt completes, success or failure --
    // mainly useful for logging/diagnostics; callers don't need to wait
    // for this before calling pickRandomInstance().
    void instanceListRefreshed(bool success);

private slots:
    void onInstancesJsonFinished();
    void onSslErrors(const QList<QSslError> &errors);

private:
    QStringList liveInstances;  // populated from api.invidious.io; empty until first successful refresh
    bool refreshInFlight;

    static QStringList parseInstancesJson(const QString &json);
};

#endif /* INVIDIOUSINSTANCEMANAGER_HPP_ */
