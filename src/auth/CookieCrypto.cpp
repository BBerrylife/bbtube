#include "CookieCrypto.hpp"

#include <bb/device/HardwareInfo>

#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include <QByteArray>

#include <string.h>

static const int AES_KEY_BITS = 256;
static const int IV_LEN = AES_BLOCK_SIZE; // 16

void CookieCrypto::deriveKey(unsigned char outKey[32])
{
    bb::device::HardwareInfo hwInfo;
    // hardwareId() needs no special permission (unlike pin()/serialNumber()/
    // imei()) -- it's shared by all devices of the same model, so it's not
    // by itself a strong per-device secret, but combined with the fact that
    // the encrypted blob never leaves this device's sqlite db in normal use,
    // it's enough to keep the cookie from being plaintext-readable in the
    // db file. See header comment for the actual threat model.
    QByteArray material = QByteArray("bbtube-google-auth-v1|") + hwInfo.hardwareId().toUtf8();

    SHA256(reinterpret_cast<const unsigned char*>(material.constData()),
            material.size(), outKey);
}

bool CookieCrypto::encrypt(const QString &plaintext, QString *outCipherTextBase64,
        QString *outIvBase64)
{
    QByteArray plainBytes = plaintext.toUtf8();

    // PKCS#7 pad to a multiple of AES_BLOCK_SIZE (always adds 1-16 bytes,
    // even if plainBytes.size() is already a multiple of the block size --
    // that's what lets decrypt() unambiguously find the padding length).
    int padLen = AES_BLOCK_SIZE - (plainBytes.size() % AES_BLOCK_SIZE);
    QByteArray padded = plainBytes;
    padded.append(QByteArray(padLen, static_cast<char>(padLen)));

    unsigned char iv[IV_LEN];
    if (RAND_bytes(iv, IV_LEN) != 1) {
        return false;
    }
    // AES_cbc_encrypt mutates the IV buffer in place as it processes each
    // block, so keep a pristine copy to store separately.
    unsigned char ivForStorage[IV_LEN];
    memcpy(ivForStorage, iv, IV_LEN);

    unsigned char key[32];
    deriveKey(key);

    AES_KEY aesKey;
    if (AES_set_encrypt_key(key, AES_KEY_BITS, &aesKey) != 0) {
        return false;
    }

    QByteArray cipherBytes(padded.size(), Qt::Uninitialized);
    AES_cbc_encrypt(
            reinterpret_cast<const unsigned char*>(padded.constData()),
            reinterpret_cast<unsigned char*>(cipherBytes.data()),
            padded.size(), &aesKey, iv, AES_ENCRYPT);

    *outCipherTextBase64 = QString::fromLatin1(cipherBytes.toBase64());
    *outIvBase64 = QString::fromLatin1(
            QByteArray(reinterpret_cast<const char*>(ivForStorage), IV_LEN).toBase64());

    return true;
}

bool CookieCrypto::decrypt(const QString &cipherTextBase64, const QString &ivBase64,
        QString *outPlaintext)
{
    outPlaintext->clear();

    QByteArray cipherBytes = QByteArray::fromBase64(cipherTextBase64.toLatin1());
    QByteArray ivBytes = QByteArray::fromBase64(ivBase64.toLatin1());

    if (ivBytes.size() != IV_LEN || cipherBytes.isEmpty()
            || cipherBytes.size() % AES_BLOCK_SIZE != 0) {
        return false; // malformed blob -- treat as "not logged in", not a crash
    }

    unsigned char iv[IV_LEN];
    memcpy(iv, ivBytes.constData(), IV_LEN);

    unsigned char key[32];
    deriveKey(key);

    AES_KEY aesKey;
    if (AES_set_decrypt_key(key, AES_KEY_BITS, &aesKey) != 0) {
        return false;
    }

    QByteArray plainBytes(cipherBytes.size(), Qt::Uninitialized);
    AES_cbc_encrypt(
            reinterpret_cast<const unsigned char*>(cipherBytes.constData()),
            reinterpret_cast<unsigned char*>(plainBytes.data()),
            cipherBytes.size(), &aesKey, iv, AES_DECRYPT);

    // Validate + strip PKCS#7 padding. If the key was wrong (e.g. db copied
    // from a different device) this will very likely fail the sanity check
    // below rather than silently produce garbage text, since a wrong key
    // makes the last byte essentially random.
    if (plainBytes.isEmpty()) {
        return false;
    }
    unsigned char padLen = static_cast<unsigned char>(plainBytes.at(plainBytes.size() - 1));
    if (padLen == 0 || padLen > AES_BLOCK_SIZE || padLen > plainBytes.size()) {
        return false;
    }
    for (int i = plainBytes.size() - padLen; i < plainBytes.size(); i++) {
        if (static_cast<unsigned char>(plainBytes.at(i)) != padLen) {
            return false;
        }
    }
    plainBytes.chop(padLen);

    *outPlaintext = QString::fromUtf8(plainBytes.constData(), plainBytes.size());
    return true;
}
