#ifndef SSLTRUST_HPP_
#define SSLTRUST_HPP_

// BB10's built-in root CA trust store was last updated around the
// device's original firmware release (~2013) and is not otherwise
// updatable by this app. Certificate authorities that didn't exist yet
// at that time -- most notably Let's Encrypt's "ISRG Root X1", first
// issued 2015-06-04 and now used by a very large fraction of the modern
// web (including api.invidious.io) -- are absent from it, so HTTPS
// connections to any site relying solely on such a CA fail with an SSL
// handshake error (QNetworkReply reports "SSL handshake failed"), even
// though the app's OpenSSL itself supports the TLS version fine.
//
// This is NOT the same class of problem as a server requiring a newer
// TLS protocol version than this OpenSSL build supports (BB10 10.3.1's
// OpenSSL does support up to TLS 1.2, matching e.g. Zalo10's server) --
// it's specifically a "don't recognize/trust this issuer" problem,
// fixable at the application level by explicitly telling Qt's SSL layer
// to also trust the missing root CA, without needing any OS/firmware
// update.
//
// Call SslTrust::installExtraRootCertificates() once, early in
// ApplicationUI's constructor, before any QNetworkAccessManager request
// is made.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class SslTrust
{
public:
    static void installExtraRootCertificates();

private:
    SslTrust(); // static-only utility class, not constructible
};

#endif /* SSLTRUST_HPP_ */
