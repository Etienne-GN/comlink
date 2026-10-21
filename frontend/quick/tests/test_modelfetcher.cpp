/* The speech model download, against local files instead of Hugging
 * Face: what lands on disk when the hash matches, when it does not,
 * and when the operator cancels. Nothing here touches the network. */
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <cstdio>

#include "modelfetcher.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s\n", #c); fails++; } \
                      else printf("  ok   %s\n", #c); } while (0)

static bool waitFor(QObject *o, const char *sig, int ms) {
    QEventLoop loop;
    QObject::connect(o, sig, &loop, SLOT(quit()));
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    QSignalSpy spy(o, sig);
    loop.exec();
    return spy.count() > 0;
}

static bool waitState(ModelFetcher &f, const QString &want, int ms) {
    for (int i = 0; i < ms / 20 && f.state() != want; i++) {
        QEventLoop l;
        QTimer::singleShot(20, &l, &QEventLoop::quit);
        l.exec();
    }
    return f.state() == want;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString root = QDir::currentPath() + "/modelfetcher-test";
    QDir(root).removeRecursively();
    QDir().mkpath(root + "/src");
    QDir().mkpath(root + "/dst");

    const QByteArray body("not really a model, but bytes all the same");
    QFile src(root + "/src/ggml-base.bin");
    if (!src.open(QIODevice::WriteOnly))
        return 1;
    src.write(body);
    src.close();
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());

    printf("matching hash lands at the final path\n");
    {
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl::fromLocalFile(root + "/src/"));
        f.setExpected("base", body.size(), sha);
        QSignalSpy done(&f, &ModelFetcher::finished);
        f.download("base");
        CHECK(done.count() == 1 || done.wait(5000));
        CHECK(QFile::exists(root + "/dst/ggml-base.bin"));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin.part"));
        CHECK(f.state() == "idle");
        bool present = false;
        for (const QVariant &v : f.models())
            if (v.toMap()["name"] == "base")
                present = v.toMap()["present"].toBool();
        CHECK(present);
        if (!done.isEmpty())
            CHECK(done.first().at(1).toString()
                  == root + "/dst/ggml-base.bin");
        CHECK(f.remove("base"));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin"));
    }

    printf("a mismatched hash leaves nothing\n");
    {
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl::fromLocalFile(root + "/src/"));
        f.setExpected("base", body.size(), QString(64, '0'));
        QSignalSpy done(&f, &ModelFetcher::finished);
        f.download("base");
        CHECK(waitState(f, "error", 5000));
        CHECK(f.error().contains("checksum"));
        CHECK(done.isEmpty());
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin"));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin.part"));
    }

    printf("cancel removes the part file\n");
    {
        /* Accepts and never answers: a download that never ends. */
        QTcpServer silent;
        CHECK(silent.listen(QHostAddress::LocalHost));
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/")
                              .arg(silent.serverPort())));
        f.setExpected("base", body.size(), sha);
        f.download("base");
        CHECK(f.state() == "downloading");
        waitFor(&silent, SIGNAL(newConnection()), 2000);
        f.cancel();
        CHECK(waitState(f, "idle", 2000));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin.part"));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin"));
    }

    printf("progress ticks do not announce a state change\n");
    {
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl::fromLocalFile(root + "/src/"));
        f.setExpected("base", body.size(), sha);
        QSignalSpy changed(&f, &ModelFetcher::changed);
        QSignalSpy done(&f, &ModelFetcher::finished);
        f.download("base");
        CHECK(done.count() == 1 || done.wait(5000));
        /* downloading, then idle: anything more reloads the page's
           model list on every tick. */
        CHECK(changed.count() == 2);
        CHECK(f.remove("base"));
    }

    printf("a later failure does not delete the model just fetched\n");
    {
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl::fromLocalFile(root + "/src/"));
        f.setExpected("base", body.size(), sha);
        QSignalSpy done(&f, &ModelFetcher::finished);
        f.download("base");
        CHECK(done.count() == 1 || done.wait(5000));
        QFile blocker(root + "/blocker");
        CHECK(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        f.setDir(root + "/blocker/models");
        f.download("small");
        CHECK(f.state() == "error");
        CHECK(QFile::exists(root + "/dst/ggml-base.bin"));
        f.setDir(root + "/dst");
        CHECK(f.remove("base"));
        QFile::remove(root + "/blocker");
    }

    printf("a stalled download gives up\n");
    {
        QTcpServer silent;
        CHECK(silent.listen(QHostAddress::LocalHost));
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/")
                              .arg(silent.serverPort())));
        f.setExpected("base", body.size(), sha);
        f.setTransferTimeout(1000);
        f.download("base");
        CHECK(waitState(f, "error", 5000));
        CHECK(f.error().contains("stalled"));
        CHECK(!QFile::exists(root + "/dst/ggml-base.bin.part"));
    }

    printf("a part file left by an earlier run is swept\n");
    {
        QFile stale(root + "/dst/ggml-small.bin.part");
        CHECK(stale.open(QIODevice::WriteOnly));
        stale.write("half a model");
        stale.close();
        ModelFetcher f;
        f.setDir(root + "/dst");
        f.setBaseUrl(QUrl::fromLocalFile(root + "/src/"));
        f.setExpected("base", body.size(), sha);
        QSignalSpy done(&f, &ModelFetcher::finished);
        f.download("base");
        CHECK(done.count() == 1 || done.wait(5000));
        CHECK(!QFile::exists(root + "/dst/ggml-small.bin.part"));
        CHECK(f.remove("base"));
    }

    printf("the real models are pinned\n");
    {
        ModelFetcher f;
        const QVariantList ms = f.models();
        CHECK(ms.size() == 2);
        CHECK(ms.value(0).toMap()["name"] == "small");
        CHECK(ms.value(1).toMap()["name"] == "base");
        CHECK(f.baseUrl().toString().contains(
            "5359861c739e955e79d9a303bcbc70fb988958b1"));
    }

    QDir(root).removeRecursively();
    return fails == 0 ? 0 : 1;
}
