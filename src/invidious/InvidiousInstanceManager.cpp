#include "InvidiousInstanceManager.hpp"
#include "src/applicationui.hpp"

#include <bb/data/JsonDataAccess>

#include <QtNetwork/QNetworkRequest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QTime>
#include <QDebug>
#include <QtNetwork/QSslCertificate>
#include <QTimer>

// Hardcoded fallback, used only until the first successful
// refreshInstanceList() -- e.g. very first app launch before any network
// call has completed, or launched with no connectivity. These are
// long-standing instances pulled from the official public list at
// https://docs.invidious.io/instances/ as of when this was written; they
// are NOT guaranteed to stay up forever, which is exactly why the live
// list from api.invidious.io (kept current by the Invidious maintainers'
// uptime/maintenance requirements) is preferred whenever available.
static const char *FALLBACK_INSTANCES[] = {
    "https://yewtu.be",
    "https://yt.artemislena.eu",
    "https://inv.nadeko.net",
    "https://inv.tux.pizza",
    "https://invidious.protokolla.fi",
};
static const int FALLBACK_INSTANCES_COUNT = 5;

InvidiousInstanceManager::InvidiousInstanceManager(QObject *parent) :
        QObject(parent), refreshInFlight(false)
{
    // Seed a random source once per app run. qsrand() is process-global in
    // Qt4, so this is intentionally only done here, not per-call.
    qsrand(static_cast<uint>(QTime::currentTime().msec()));
}

void InvidiousInstanceManager::refreshInstanceList()
{
    if (refreshInFlight) {
        return;
    }
    refreshInFlight = true;

    QNetworkRequest request(QString("https://api.invidious.io/instances.json?sort_by=type"));
    // Identify plainly rather than spoofing a browser -- this is a
    // metadata/discovery request, not a YouTube-facing one, so there's no
    // anti-bot concern here.
    request.setRawHeader("User-Agent", "bbtube (BlackBerry 10)");

    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onInstancesJsonFinished()));
    QObject::connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this,
            SLOT(onSslErrors(QList<QSslError>)));

    // Qt4's QNetworkAccessManager on this BB10 build has no built-in
    // per-request timeout -- a stalled connection (e.g. a slow/broken
    // TLS handshake) would otherwise leave finished() never firing,
    // hanging refreshInstanceList() (and by extension, anything waiting
    // on it) indefinitely. 10s is generous for a small JSON fetch.
    QTimer::singleShot(10000, reply, SLOT(abort()));
}

void InvidiousInstanceManager::onSslErrors(const QList<QSslError> &errors)
{
    // QNetworkReply::errorString() alone just says "SSL handshake
    // failed" with no detail on WHY. This logs the actual QSslError
    // reason(s) -- e.g. CertificateUntrusted, HostNameMismatch,
    // CertificateExpired, SelfSignedCertificate -- so a fetch failure
    // can actually be diagnosed instead of guessed at.
    for (int i = 0; i < errors.count(); i++) {
        qDebug() << "[bbtube][invidious][ssl] error" << i << ":" << errors[i].errorString()
                 << "(code" << errors[i].error() << ")";
        QSslCertificate cert = errors[i].certificate();
        if (!cert.isNull()) {
            qDebug() << "[bbtube][invidious][ssl]   certificate subject:"
                     << cert.subjectInfo(QSslCertificate::CommonName) << ", issuer:"
                     << cert.issuerInfo(QSslCertificate::CommonName) << ", expires:"
                     << cert.expiryDate().toString();
        }
    }
}

void InvidiousInstanceManager::onInstancesJsonFinished()
{
    refreshInFlight = false;

    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QVariant httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        qDebug() << "[bbtube][invidious] instances.json fetch failed. url:" << reply->url().toString()
                 << ", error code:" << reply->error()
                 << ", error string:" << reply->errorString()
                 << ", http status:" << httpStatus;
        emit instanceListRefreshed(false);
        return;
    }

    QString json = QString::fromUtf8(reply->readAll());
    QStringList parsed = parseInstancesJson(json);

    if (parsed.isEmpty()) {
        qDebug() << "[bbtube][invidious] instances.json parsed to zero usable instances";
        emit instanceListRefreshed(false);
        return;
    }

    liveInstances = parsed;
    qDebug() << "[bbtube][invidious] refreshed instance list, count =" << liveInstances.count();
    emit instanceListRefreshed(true);
}

QStringList InvidiousInstanceManager::parseInstancesJson(const QString &json)
{
    QStringList result;

    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(json);
    QVariantList entries = parsed.toList();

    // Each entry is a 2-element array: [hostname, {detail object}]. See
    // https://api.invidious.io/ and the "Instances API" docs -- this
    // unusual [name, detail] pairing (rather than a plain object keyed by
    // hostname) is how the Invidious project's own tooling consumes it
    // (see e.g. https://github.com/tatsumoto-ren/dotfiles's
    // rank-invidious-instances script, which indexes d[1]['uri'] /
    // d[1]['monitor']['uptime']).
    for (int i = 0; i < entries.count(); i++) {
        QVariantList entry = entries[i].toList();
        if (entry.count() < 2) {
            continue;
        }
        QVariantMap detail = entry[1].toMap();

        QString type = detail["type"].toString();
        if (type != "https") {
            // Skip onion/i2p/http-only entries -- BB10's network stack
            // and this app's use case (plain HTTPS JSON + video URLs)
            // has no use for Tor/I2P hidden services.
            continue;
        }

        QString uri = detail["uri"].toString();
        if (uri.isEmpty()) {
            continue;
        }
        // Normalize: no trailing slash, so callers can always do
        // instanceBaseUrl + "/api/v1/...".
        while (uri.endsWith("/")) {
            uri.chop(1);
        }

        result.append(uri);
    }

    return result;
}

QString InvidiousInstanceManager::pickRandomInstance() const
{
    if (!liveInstances.isEmpty()) {
        int index = qrand() % liveInstances.count();
        return liveInstances[index];
    }

    if (FALLBACK_INSTANCES_COUNT == 0) {
        return "";
    }
    int index = qrand() % FALLBACK_INSTANCES_COUNT;
    return QString(FALLBACK_INSTANCES[index]);
}
