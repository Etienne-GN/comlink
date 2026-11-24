#include "backup.h"

#include <QCoreApplication>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

const QByteArray MAGIC = QByteArrayLiteral("COMLINK-BACKUP-SEALED-1\n");
const int SALT_LEN = 16;
const int NONCE_LEN = 12;
const int TAG_LEN = 16;
const int KEY_LEN = 32;
const int KDF_ROUNDS = 600000;

QString tr(const char *s) {
    return QCoreApplication::translate("Backup", s);
}

bool derive(const QString &passphrase, const QByteArray &salt,
            unsigned char *key) {
    const QByteArray p = passphrase.toUtf8();
    return PKCS5_PBKDF2_HMAC(p.constData(), (int)p.size(),
                             (const unsigned char *)salt.constData(),
                             (int)salt.size(), KDF_ROUNDS, EVP_sha256(),
                             KEY_LEN, key) == 1;
}

struct Ctx {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    ~Ctx() { EVP_CIPHER_CTX_free(c); }
};

}  // namespace

bool Backup::sealed(const QByteArray &file) {
    return file.startsWith(MAGIC);
}

QByteArray Backup::seal(const QByteArray &plain, const QString &passphrase,
                        QString *why) {
    QByteArray salt(SALT_LEN, 0), nonce(NONCE_LEN, 0);
    unsigned char key[KEY_LEN];
    Ctx ctx;
    QByteArray ct(plain.size() + 16, 0);
    unsigned char tag[TAG_LEN];
    int n = 0, tail = 0;

    /* The format line is authenticated with the data, so a sealed
       file cannot be relabelled as another version of itself. */
    const bool ok =
        ctx.c
        && RAND_bytes((unsigned char *)salt.data(), SALT_LEN) == 1
        && RAND_bytes((unsigned char *)nonce.data(), NONCE_LEN) == 1
        && derive(passphrase, salt, key)
        && EVP_EncryptInit_ex(ctx.c, EVP_aes_256_gcm(), nullptr, key,
                              (const unsigned char *)nonce.constData()) == 1
        && EVP_EncryptUpdate(ctx.c, nullptr, &n,
                             (const unsigned char *)MAGIC.constData(),
                             (int)MAGIC.size()) == 1
        && EVP_EncryptUpdate(ctx.c, (unsigned char *)ct.data(), &n,
                             (const unsigned char *)plain.constData(),
                             (int)plain.size()) == 1
        && EVP_EncryptFinal_ex(ctx.c, (unsigned char *)ct.data() + n,
                               &tail) == 1
        && EVP_CIPHER_CTX_ctrl(ctx.c, EVP_CTRL_GCM_GET_TAG, TAG_LEN,
                               tag) == 1;
    if (!ok) {
        if (why)
            *why = tr("The backup could not be sealed.");
        return QByteArray();
    }
    ct.truncate(n + tail);
    return MAGIC + salt + nonce + ct
           + QByteArray((const char *)tag, TAG_LEN);
}

QByteArray Backup::unseal(const QByteArray &file, const QString &passphrase,
                          QString *why) {
    const qsizetype head = MAGIC.size() + SALT_LEN + NONCE_LEN;
    if (!sealed(file) || file.size() < head + TAG_LEN) {
        if (why)
            *why = tr("That is not a sealed Comlink backup.");
        return QByteArray();
    }
    const QByteArray salt = file.mid(MAGIC.size(), SALT_LEN);
    const QByteArray nonce = file.mid(MAGIC.size() + SALT_LEN, NONCE_LEN);
    const QByteArray ct = file.mid(head, file.size() - head - TAG_LEN);
    QByteArray tag = file.right(TAG_LEN);

    unsigned char key[KEY_LEN];
    Ctx ctx;
    QByteArray plain(ct.size() + 16, 0);
    int n = 0, tail = 0;
    const bool ok =
        ctx.c
        && derive(passphrase, salt, key)
        && EVP_DecryptInit_ex(ctx.c, EVP_aes_256_gcm(), nullptr, key,
                              (const unsigned char *)nonce.constData()) == 1
        && EVP_DecryptUpdate(ctx.c, nullptr, &n,
                             (const unsigned char *)MAGIC.constData(),
                             (int)MAGIC.size()) == 1
        && EVP_DecryptUpdate(ctx.c, (unsigned char *)plain.data(), &n,
                             (const unsigned char *)ct.constData(),
                             (int)ct.size()) == 1
        && EVP_CIPHER_CTX_ctrl(ctx.c, EVP_CTRL_GCM_SET_TAG, TAG_LEN,
                               tag.data()) == 1
        && EVP_DecryptFinal_ex(ctx.c, (unsigned char *)plain.data() + n,
                               &tail) == 1;
    if (!ok) {
        if (why)
            *why = tr("Wrong passphrase, or the backup is damaged.");
        return QByteArray();
    }
    plain.truncate(n + tail);
    return plain;
}
