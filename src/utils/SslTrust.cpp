#include "SslTrust.hpp"

#include <QtNetwork/QSslSocket>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslCertificate>
#include <QByteArray>
#include <QList>
#include <QDebug>
#include <QDateTime>

// ISRG Root X1 (Let's Encrypt's self-signed root CA), PEM-encoded.
// Source: certifi's cacert.pem (github.com/certifi/python-certifi),
// cross-checked against the official download at
// https://letsencrypt.org/certs/isrgrootx1.pem and its published
// SHA-256 fingerprint (96:bc:ec:06:26:49:76:f3:74:60:77:9a:cf:28:c5:a7:
// cf:e8:a3:c0:aa:e1:1a:8f:fc:ee:05:c0:bd:df:08:c6). Self-signed,
// NotBefore 2015-06-04, NotAfter 2035-06-04 -- no expiry concern for the
// foreseeable lifetime of this app.
//
// This is a small, fixed, publicly-published root certificate -- not a
// secret, not something that needs updating at runtime.
static const char *ISRG_ROOT_X1_PEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

void SslTrust::installExtraRootCertificates()
{
    QList<QSslCertificate> systemCerts = QSslSocket::systemCaCertificates();
    qDebug() << "[bbtube][ssl] system CA certificates found:" << systemCerts.count();
    for (int i = 0; i < systemCerts.count(); i++) {
        qDebug() << "[bbtube][ssl]   system CA" << i << ":"
                 << systemCerts[i].subjectInfo(QSslCertificate::CommonName)
                 << "(issuer:" << systemCerts[i].issuerInfo(QSslCertificate::CommonName)
                 << ", expires:" << systemCerts[i].expiryDate().toString() << ")";
    }

    QSslCertificate isrgRootX1(QByteArray(ISRG_ROOT_X1_PEM), QSsl::Pem);

    if (isrgRootX1.isNull()) {
        // Shouldn't happen (this is a fixed, verified-good PEM blob),
        // but don't silently proceed with a broken config if it somehow
        // does.
        qDebug() << "[bbtube][ssl] failed to parse embedded ISRG Root X1 certificate";
        return;
    }
    qDebug() << "[bbtube][ssl] parsed ISRG Root X1 OK, subject:"
             << isrgRootX1.subjectInfo(QSslCertificate::CommonName)
             << "expires:" << isrgRootX1.expiryDate().toString();

    // IMPORTANT: QSslSocket::defaultCaCertificates() (the store actually
    // used to verify handshakes) is NOT pre-populated with the OS trust
    // store on this BB10/Qt4 build -- confirmed via logging, it starts
    // EMPTY. QSslSocket::systemCaCertificates() (queried above into
    // systemCerts) DOES return the 125 OS-trusted roots, but that's a
    // separate, read-only snapshot that nothing wires into the verification
    // path automatically.
    //
    // A previous version of this function called
    // addDefaultCaCertificates() with only the embedded ISRG Root X1 cert,
    // assuming it would be appended to an already-populated OS store.
    // Because the real store was empty, that left exactly ONE trusted CA
    // in the whole app (confirmed via logging: "default CA certificates
    // after install: 1"). That broke TLS to every HTTPS host whose chain
    // doesn't lead to Let's Encrypt -- notably *.googlevideo.com, which
    // chains through Google/GTS or DigiCert roots, not ISRG Root X1. Direct
    // video stream fetches (HEAD/GET against googlevideo.com, used by the
    // remuxer regardless of requested resolution) then failed SSL
    // handshake, while plain-HTTP calls like the Invidious instance
    // requests kept working since they never touch TLS at all.
    //
    // Fix: explicitly seed the default store with the full system CA list
    // *plus* the embedded ISRG Root X1 (kept in case the system store is
    // missing/outdated on some devices), rather than relying on any
    // implicit merge.
    QList<QSslCertificate> toAdd = systemCerts;
    toAdd.append(isrgRootX1);
    QSslSocket::addDefaultCaCertificates(toAdd);

    QList<QSslCertificate> afterCerts = QSslSocket::defaultCaCertificates();
    qDebug() << "[bbtube][ssl] default CA certificates after install:" << afterCerts.count();
    for (int i = 0; i < afterCerts.count(); i++) {
        qDebug() << "[bbtube][ssl]   default CA" << i << ":"
                 << afterCerts[i].subjectInfo(QSslCertificate::CommonName);
    }

    // Also make sure new QSslConfiguration-based requests negotiate the
    // highest TLS version this OpenSSL build supports, rather than
    // whatever Qt's own baked-in default protocol enum value is.
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    qDebug() << "[bbtube][ssl] defaultConfiguration() protocol before:" << config.protocol();
    config.setProtocol(QSsl::SecureProtocols);
    QSslConfiguration::setDefaultConfiguration(config);
    qDebug() << "[bbtube][ssl] defaultConfiguration() protocol after:"
             << QSslConfiguration::defaultConfiguration().protocol();
}
