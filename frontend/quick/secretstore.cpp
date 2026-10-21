#include "secretstore.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#else
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#endif

#include <QCoreApplication>
#include <QMap>
#include <QVariantMap>

#ifndef Q_OS_ANDROID
/* The Secret Service's secret: a session, whatever parameters the
   session's algorithm needs, the bytes, and what they are. Registered
   with the D-Bus type system because it appears inside a variant and
   Qt cannot marshal it otherwise. */
struct SecretServiceSecret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};
Q_DECLARE_METATYPE(SecretServiceSecret)

QDBusArgument &operator<<(QDBusArgument &a, const SecretServiceSecret &s) {
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.contentType;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a,
                                SecretServiceSecret &s) {
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.contentType;
    a.endStructure();
    return a;
}

namespace {

const char *SERVICE = "org.freedesktop.secrets";
const char *PATH = "/org/freedesktop/secrets";
const char *IFACE = "org.freedesktop.Secret.Service";
const char *COLLECTION = "/org/freedesktop/secrets/aliases/default";

/* One session per process, opened on first use.
 *
 * "plain" rather than a Diffie-Hellman session: the secret travels
 * over the session bus, which is a unix socket only this user can
 * open, and the daemon is this user's own process. Encrypting the
 * hop would protect against nothing that is not already inside the
 * trust boundary, and dh-ietf1024-sha256-aes128-cbc-pkcs7 is a lot of
 * code to get subtly wrong for that. */
QDBusObjectPath openSession(bool *ok) {
    static bool tried = false;
    static bool good = false;
    static QDBusObjectPath session;
    if (tried) {
        if (ok) *ok = good;
        return session;
    }
    tried = true;

    qDBusRegisterMetaType<SecretServiceSecret>();
    qDBusRegisterMetaType<QMap<QString, QString>>();
    QDBusInterface svc(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    if (!svc.isValid()) {
        if (ok) *ok = false;
        return session;
    }
    QDBusMessage reply = svc.call("OpenSession", QString("plain"),
                                  QVariant::fromValue(
                                      QDBusVariant(QString())));
    if (reply.type() != QDBusMessage::ReplyMessage
        || reply.arguments().size() < 2) {
        if (ok) *ok = false;
        return session;
    }
    session = qvariant_cast<QDBusObjectPath>(reply.arguments().at(1));
    good = !session.path().isEmpty();
    if (ok) *ok = good;
    return session;
}

/* The attributes an item is found by. The application name is part of
   them so this never reads, still less overwrites, somebody else's
   password for the same account name.
 *
 * QMap<QString, QString>, not QVariantMap: the Secret Service's
 * attributes are a{ss}, and a QVariantMap marshals as a{sv}, which
 * the daemon rejects as the wrong signature. */
QMap<QString, QString> attributesFor(const QString &key) {
    QMap<QString, QString> a;
    a["application"] = QStringLiteral("comlink");
    a["key"] = key;
    return a;
}

QList<QDBusObjectPath> search(const QString &key) {
    QList<QDBusObjectPath> found;
    QDBusInterface svc(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    if (!svc.isValid())
        return found;
    QDBusMessage reply = svc.call("SearchItems",
                                  QVariant::fromValue(attributesFor(key)));
    if (reply.type() != QDBusMessage::ReplyMessage)
        return found;
    /* Unlocked first, then locked: a locked item is still the right
       item, and reading it is what prompts the keyring to unlock. */
    for (const QVariant &v : reply.arguments()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        QList<QDBusObjectPath> paths;
        arg >> paths;
        found += paths;
    }
    return found;
}

}  // namespace
#endif  /* !Q_OS_ANDROID */

bool SecretStore::available() {
#ifdef Q_OS_ANDROID
    /* The keystore is part of the platform; there is nothing to be
       unavailable. A failure to encrypt is reported per operation. */
    return true;
#else
    bool ok = false;
    openSession(&ok);
    return ok;
#endif
}

/* Whether the store answered at all. Distinct from describe(), which
   names it. */
QString SecretStore::describe() {
#ifdef Q_OS_ANDROID
    return QStringLiteral("the Android Keystore");
#else
    return available()
               ? QStringLiteral("the desktop keyring (Secret Service)")
               : QStringLiteral("the configuration file, in cleartext: "
                                "no keyring answered");
#endif
}

#ifdef Q_OS_ANDROID

bool SecretStore::store(const QString &key, const QString &value) {
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return false;
    /* Java answers whether the keystore took it. It used to be a void
       call that could not fail, while Secrets.store quietly *deleted*
       the entry on an exception -- so a keystore failure erased the
       password and reported success. */
    return QJniObject::callStaticMethod<jboolean>(
        "io/github/etienne_gn/comlink/Secrets", "set",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
        ctx.object(), QJniObject::fromString(key).object<jstring>(),
        QJniObject::fromString(value).object<jstring>());
}

QString SecretStore::load(const QString &key, bool *found) {
    if (found) *found = false;
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return QString();
    QJniObject out = QJniObject::callStaticObjectMethod(
        "io/github/etienne_gn/comlink/Secrets", "get",
        "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
        ctx.object(), QJniObject::fromString(key).object<jstring>());
    /* Secrets.get answers "" both for nothing stored and for
       something that would not decrypt, and it clears the entry in
       the second case -- so by the time this returns, "" is the
       truth either way. */
    const QString v = out.isValid() ? out.toString() : QString();
    if (found) *found = true;
    return v;
}

bool SecretStore::erase(const QString &key) {
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return false;
    QJniObject::callStaticMethod<void>(
        "io/github/etienne_gn/comlink/Secrets", "remove",
        "(Landroid/content/Context;Ljava/lang/String;)V",
        ctx.object(), QJniObject::fromString(key).object<jstring>());
    return true;
}

#else

bool SecretStore::store(const QString &key, const QString &value) {
    bool ok = false;
    const QDBusObjectPath session = openSession(&ok);
    if (!ok)
        return false;

    QDBusInterface coll(SERVICE, COLLECTION,
                        "org.freedesktop.Secret.Collection",
                        QDBusConnection::sessionBus());
    if (!coll.isValid())
        return false;

    QVariantMap properties;
    properties["org.freedesktop.Secret.Item.Label"] =
        QStringLiteral("Comlink: %1").arg(key);
    properties["org.freedesktop.Secret.Item.Attributes"] =
        QVariant::fromValue(attributesFor(key));

    SecretServiceSecret secret;
    secret.session = session;
    secret.value = value.toUtf8();
    secret.contentType = QStringLiteral("text/plain; charset=utf8");

    /* replace = true: an account whose password changed must not
       leave the old one behind for somebody to find. */
    QDBusMessage reply = coll.call("CreateItem", properties,
                                   QVariant::fromValue(secret), true);
    if (reply.type() != QDBusMessage::ReplyMessage
        || reply.arguments().size() < 2)
        return false;

    /* A locked collection answers successfully and stores nothing: it
       returns item "/" and a prompt the caller is expected to drive.
       Taking the reply itself for success is how a password goes
       missing while the window says it was kept. */
    const QDBusObjectPath item =
        qvariant_cast<QDBusObjectPath>(reply.arguments().at(0));
    const QDBusObjectPath prompt =
        qvariant_cast<QDBusObjectPath>(reply.arguments().at(1));
    if (item.path().isEmpty() || item.path() == QLatin1String("/")) {
        if (!prompt.path().isEmpty() && prompt.path() != QLatin1String("/"))
            /* Driving a prompt means showing the keyring's own unlock
               dialog and waiting on its signal. Refusing is honest
               and the window says why; pretending is not. */
            return false;
        return false;
    }
    return true;
}

QString SecretStore::load(const QString &key, bool *found) {
    if (found) *found = false;
    bool ok = false;
    const QDBusObjectPath session = openSession(&ok);
    if (!ok)
        return QString();

    const QList<QDBusObjectPath> items = search(key);
    /* No item at all is "there is no password", which is a fact. An
       item that would not answer is "this could not be read", which
       is not -- and the two must not look the same to the caller. */
    bool sawItem = false;
    for (const QDBusObjectPath &item : items) {
        sawItem = true;
        QDBusInterface it(SERVICE, item.path(),
                          "org.freedesktop.Secret.Item",
                          QDBusConnection::sessionBus());
        if (!it.isValid())
            continue;
        QDBusMessage reply = it.call("GetSecret",
                                     QVariant::fromValue(session));
        /* A locked item answers Secret.Error.IsLocked; unlocking it
           needs Unlock and a prompt, which is a dialog this has no
           business raising from a settings pane. */
        if (reply.type() != QDBusMessage::ReplyMessage
            || reply.arguments().isEmpty())
            continue;
        SecretServiceSecret out;
        reply.arguments().at(0).value<QDBusArgument>() >> out;
        if (!out.value.isEmpty()) {
            if (found) *found = true;
            return QString::fromUtf8(out.value);
        }
    }
    /* No item at all is a definitive "there is no password". An item
       that exists but would not answer is not definitive, and found
       stays false so nothing downstream takes the empty string for a
       fact. */
    if (found && !sawItem)
        *found = true;
    return QString();
}

bool SecretStore::erase(const QString &key) {
    bool ok = false;
    openSession(&ok);
    if (!ok)
        return false;
    bool any = false;
    for (const QDBusObjectPath &item : search(key)) {
        QDBusInterface it(SERVICE, item.path(),
                          "org.freedesktop.Secret.Item",
                          QDBusConnection::sessionBus());
        if (it.isValid()
            && it.call("Delete").type() == QDBusMessage::ReplyMessage)
            any = true;
    }
    return any;
}

#endif
