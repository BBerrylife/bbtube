#ifndef COOKIECRYPTO_HPP_
#define COOKIECRYPTO_HPP_

#include <QString>

// Encrypts/decrypts the stored Google session cookie string using
// AES-256-CBC (OpenSSL, part of the BB10/QNX NDK -- see openssl/aes.h,
// openssl/sha.h, openssl/rand.h). Requires linking -lcrypto (see
// Youtube.pro).
//
// The AES key is never stored anywhere -- it's derived at runtime by
// SHA-256 hashing a fixed app-specific string together with the device's
// bb::device::HardwareInfo::hardwareId(). This means the encrypted blob
// in the DB is only meaningful on the device that created it (copying
// bbtube.db to another BB10 device won't let it decrypt the cookie there).
// This is NOT meant to defend against someone with full access to a
// rooted/debuggable copy of this device -- it's meant to keep the cookie
// out of plain sight in the sqlite file (e.g. if the db is casually
// copied off the device without the hardwareId, or read by another app).
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class CookieCrypto
{
public:
    // Encrypts `plaintext`. On success returns true and fills
    // outCipherTextBase64 / outIvBase64 (both safe to store as TEXT).
    // On failure (extremely unlikely -- only if the platform RNG fails)
    // returns false and leaves the out-params untouched.
    static bool encrypt(const QString &plaintext, QString *outCipherTextBase64,
            QString *outIvBase64);

    // Decrypts a blob previously produced by encrypt(). Returns true and
    // fills outPlaintext on success. Returns false (and clears
    // outPlaintext) if the blob is malformed, corrupt, or was encrypted
    // under a different device key (e.g. db copied from another device) --
    // callers should treat that the same as "not logged in" and prompt the
    // user to log in again, rather than surfacing a crypto error.
    static bool decrypt(const QString &cipherTextBase64, const QString &ivBase64,
            QString *outPlaintext);

private:
    // Derives the 32-byte AES-256 key: SHA256("bbtube-google-auth-v1|" + hardwareId).
    static void deriveKey(unsigned char outKey[32]);

    CookieCrypto(); // static-only utility class, not constructible
};

#endif /* COOKIECRYPTO_HPP_ */
