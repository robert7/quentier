#ifndef __QUTE_NOTE__CORE__TOOLS__ENCRYPTION_MANAGER_PRIVATE_H
#define __QUTE_NOTE__CORE__TOOLS__ENCRYPTION_MANAGER_PRIVATE_H

#include <QString>

// Evernote service defined constants
#define EN_ITERATIONS (50000)
#define EN_AES_KEYSIZE (16)
#define EN_RC2_KEYSIZE (8)
#define EN_AES_HMACSIZE (32)
#define EN_RC2_HMACSIZE (16)
#define EN_IDENT "ENC0"
#define MAX_PADDING_LEN (16)

namespace qute_note {

class EncryptionManagerPrivate
{
public:
    EncryptionManagerPrivate();
    ~EncryptionManagerPrivate();

    bool decrypt(const QString & encryptedText, const QString & passphrase,
                 const QString & cipher, const size_t keyLength,
                 QString & decryptedText, QString & errorDescription);

    bool encrypt(const QString & textToEncrypt, const QString & passphrase,
                 QString & cipher, size_t & keyLength,
                 QString & encryptedText, QString & errorDescription);

private:
    // AES encryption/decryption routines
    struct SaltKind
    {
        enum type {
            SALT = 0,
            SALTMAC,
            IV
        };
    };

    bool generateSalt(const SaltKind::type saltKind, const size_t saltSize, QString & errorDescription);

    bool generateKey(const QByteArray & passphraseData, const unsigned char * salt,
                     const size_t keySize, QString & errorDescription);

    bool calculateHmac(const QByteArray & passphraseData, const unsigned char * salt,
                       const QByteArray & textToEncryptData, const size_t keySize,
                       QString & errorDescription);

    bool encyptWithAes(const QByteArray & textToEncrypt, QByteArray & encryptedText,
                       QString & errorDescription);

    bool decryptAes(const QString & encryptedText, const QString & passphrase,
                    QByteArray & decryptedText, QString & errorDescription);

    bool splitEncryptedData(const QString & encryptedData, const size_t saltSize,
                            const size_t hmacSize, QByteArray & encryptedText,
                            QString & errorDescription);

private:
    // RC2 decryption routines
    bool decryptRc2(const QString & encryptedText, const QString & passphrase,
                    QByteArray & decryptedText, QString & errorDescription);

    QByteArray rc2KeyFromPassphrase(const QString & passphrase) const;
    QString decryptRc2Chunk(const QByteArray & input, const QByteArray & xkey) const;

private:
    unsigned char m_salt[EN_AES_KEYSIZE];
    unsigned char m_saltmac[EN_AES_KEYSIZE];
    unsigned char m_iv[EN_AES_KEYSIZE];

    unsigned char m_key[EN_AES_KEYSIZE];
    unsigned char m_hmac[EN_AES_HMACSIZE];
};

} // namespace qute_note

#endif // __QUTE_NOTE__CORE__TOOLS__ENCRYPTION_MANAGER_PRIVATE_H
