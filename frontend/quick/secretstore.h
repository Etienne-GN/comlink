#ifndef COMLINK_SECRETSTORE_H
#define COMLINK_SECRETSTORE_H

#include <QString>

/* Where a SIP password lives.
 *
 * The core writes everything it is given into comlink.yaml, password
 * and all -- the right default for a desktop configuration an
 * operator edits by hand, and the wrong one for a handset in a
 * pocket. sp_account_add takes the account without a password and
 * sp_account_set_password gives it one at run time, so the file holds
 * an account that can register and no secret that can be read.
 *
 * Two backends, one per platform, because there is one honest answer
 * on each:
 *
 *   Android   AES/GCM under a key that never leaves the Android
 *             Keystore, which is what the Java frontend already did.
 *             Hardware-backed where there is a TEE.
 *   Debian    the Secret Service, over D-Bus -- gnome-keyring or
 *             KWallet. The desktop's own credential store, unlocked
 *             with the login.
 *
 * Neither protects against code running as this application: anything
 * in this process can ask the store to decrypt, exactly as this does.
 * What they buy is protection at rest -- a backup, a stolen handset
 * examined offline, a config file read by something else.
 *
 * When no store answers -- no keyring running, a headless session --
 * nothing is stored and available() is false. The caller then has the
 * choice the core always had, and the window says which it took: a
 * silent fall back to cleartext would be the worst of the three. */
class SecretStore {
public:
    /* Whether a real store answered. Probed once, on first use. */
    static bool available();
    /* What answered, for the window to show. Never empty. */
    static QString describe();

    static bool store(const QString &key, const QString &value);
    /* found says whether the store answered at all, which is a
       different fact from "there is no password". A read that failed
       must never be mistaken for an empty one: the caller would take
       the empty field it prefilled, save it, and erase the password
       it could not read. */
    static QString load(const QString &key, bool *found = nullptr);
    static bool erase(const QString &key);
};

#endif /* COMLINK_SECRETSTORE_H */
