/* The upgrade path: a configuration that predates the keyring.
 *
 * Every existing installation has its SIP password in comlink.yaml,
 * because that is where the core has always written it. The keyring
 * has nothing for those accounts. What must not happen is what did:
 * the account editor read the store, found nothing, showed an empty
 * password field, and saving erased the password that was in the file
 * -- on the operator's first look at the new window.
 *
 * Its own process and its own configuration, because one sp_core may
 * be started per process and this one has to start from a file the
 * test wrote. */
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QVariantMap>
#include <cstdio>

#include "corebridge.h"
#include "secretstore.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s\n", #c); fails++; } \
                      else printf("  ok   %s\n", #c); } while (0)

static const char *PASSWORD = "in-the-yaml-since-before-the-keyring";

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QDir::current().filePath("migration-probe");
    QDir().mkpath(dir);
    const QString cfg = dir + "/comlink.yaml";
    QFile::remove(cfg);

    /* A configuration as the Widgets window left it: the account and
       its password, together, in the file. */
    {
        QFile f(cfg);
        if (!f.open(QIODevice::WriteOnly)) {
            printf("  FAIL could not write %s\n", qPrintable(cfg));
            return 1;
        }
        f.write("log_level: 0\nsip_port: 0\n");
        f.write(QStringLiteral("db_file: %1/comlink.db\n").arg(dir).toUtf8());
        f.write("accounts:\n"
                "  - id: legacy\n"
                "    user: \"1000\"\n"
                "    domain: 127.0.0.1\n"
                "    transport: udp\n"
                "    register: false\n");
        f.write(QStringLiteral("    password: %1\n")
                    .arg(QString::fromUtf8(PASSWORD)).toUtf8());
    }
    /* Nothing in the store for it, which is the whole point. */
    SecretStore::erase(QStringLiteral("account/legacy"));

    CoreBridge::setConfigPath(cfg);
    CoreBridge b;
    CHECK(b.start());

    const QVariantList accounts = b.accounts();
    CHECK(accounts.size() == 1);
    const qlonglong id = accounts.isEmpty()
                             ? 0 : accounts.at(0).toMap()["id"].toLongLong();
    CHECK(id != 0);

    printf("the editor shows the password the file still holds\n");
    QVariantMap form = b.accountCfg(id);
    CHECK(form["password"].toString() == QString::fromUtf8(PASSWORD));
    CHECK(form["passwordKnown"].toBool());

    printf("and saving moves it out of the file\n");
    const qlonglong after = b.saveAccount(id, form);
    CHECK(after != 0);

    QFile check(cfg);
    CHECK(check.open(QIODevice::ReadOnly));
    const QByteArray written = check.readAll();
    check.close();
    if (b.credentialStoreAvailable()) {
        CHECK(!written.contains(PASSWORD));
        CHECK(SecretStore::load(QStringLiteral("account/legacy"))
              == QString::fromUtf8(PASSWORD));
        /* And it still reads back, from its new home. */
        CHECK(b.accountCfg(after)["password"].toString()
              == QString::fromUtf8(PASSWORD));
        SecretStore::erase(QStringLiteral("account/legacy"));
    } else {
        /* With no store the file keeps it, which is the old
           behaviour and still correct. */
        CHECK(written.contains(PASSWORD));
    }

    printf("\n%s\n", fails == 0 ? "migration checks passed"
                                : "MIGRATION CHECKS FAILED");
    return fails == 0 ? 0 : 1;
}
