/* The bridge's data paths, checked rather than eyeballed.
 *
 * Every pane reads the core through one of these calls, and a
 * screenshot proves only that something rendered. This proves the
 * values are the core's: the core's presets and not a list kept here,
 * a preset that saves and reads back with both its headers, go and
 * stop that differ, and the monogram rule that lives in the core.
 *
 * It runs headless with its own configuration, so it touches nothing
 * the operator owns. */
#include <QCoreApplication>
#include <QtGlobal>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <cstdio>
#include "corebridge.h"
#include "secretstore.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s\n", #c); fails++; } \
                      else printf("  ok   %s\n", #c); } while (0)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    CoreBridge b;
    CHECK(b.start());

    printf("presets\n");
    QStringList hs;
    hs << "X-Sp-Adhoc|from-test" << "X-Other|second";
    CHECK(b.savePreset("nights", hs));
    const QVariantList ps = b.headerPresets();
    CHECK(!ps.isEmpty());
    bool found = false;
    for (const QVariant &v : ps)
        if (v.toMap()["name"].toString() == "nights") {
            found = true;
            CHECK(v.toMap()["headers"].toStringList().size() == 2);
        }
    CHECK(found);
    CHECK(b.deletePreset("nights"));

    printf("theme\n");
    const QVariantList presets = b.themePresets();
    CHECK(presets.size() == sp_theme_preset_count());
    b.setPreset(5);
    CHECK(b.currentPreset() == 5);
    CHECK(b.accent().isValid());
    /* QML reads channels off these, and a string has none. */
    for (const char *n : {"accent", "background", "surface", "foreground",
                          "muted", "go", "stop", "txTrace", "rxTrace"})
        CHECK(b.property(n).metaType() == QMetaType::fromType<QColor>());
    CHECK(b.go() != b.stop());
    CHECK(b.txTrace() != b.rxTrace());
    b.setPreset(0);

    printf("codecs and modules\n");
    CHECK(!b.codecs().isEmpty());
    CHECK(!b.modules().isEmpty());
    CHECK(!b.dsp().isEmpty());
    CHECK(b.recordingSettings().contains("format"));
    CHECK(!b.audioDevices().isEmpty());

    printf("transcripts\n");
    {
        for (const QVariant &v : b.history(5))
            CHECK(v.toMap().contains("transcript"));
        CHECK(b.transcript(-1).isEmpty());
        /* This build has whisper, or the package would not depend on
           it; the handset build is the one that might not. */
#ifdef SP_TEST_EXPECT_WHISPER
        CHECK(b.transcriptAvailable());
#endif
        CHECK(b.transcriptText(-1).isEmpty());
        CHECK(!b.setTranscriptModel("ggml-small.bin").isEmpty());
        CHECK(!b.setTranscriptModel("/nonexistent/ggml-small.bin").isEmpty());
        const QString model = QDir::currentPath() + "/bridge-model.bin";
        QFile mf(model);
        CHECK(mf.open(QIODevice::WriteOnly));
        mf.write("x");
        mf.close();
        CHECK(b.setTranscriptModel(model).isEmpty());
        CHECK(b.transcriptModel() == model);
        CHECK(b.setTranscriptModel(QString()).isEmpty());
        CHECK(b.transcriptModel().isEmpty());
        QFile::remove(model);
    }

    printf("window size\n");
    b.saveWindowSize(420, 700);
    const QVariantMap g = b.windowSize();
    CHECK(g["width"].toInt() == 420 && g["height"].toInt() == 700);

    printf("accounts, plural\n");
    {
        /* This configuration survives between runs, so a failed run
           must not poison the next one. */
        for (const QVariant &v : b.accounts())
            b.removeAccount(v.toMap()["id"].toLongLong());
        CHECK(b.accounts().isEmpty());

        QVariantMap f;
        f["id"] = "work";
        f["user"] = "1000";
        f["domain"] = "127.0.0.1";
        f["transport"] = 0;
        f["persist"] = true;
        /* No registrar is listening in a unit test, and the core
           refuses a registering account with no password. */
        f["doRegister"] = false;
        CHECK(b.validateAccount(f).isEmpty());

        QVariantMap bad = f;
        bad["domain"] = "";
        CHECK(bad.size() > 0 && !b.validateAccount(bad).isEmpty());

        const qlonglong id = b.saveAccount(0, f);
        CHECK(id != 0);
        QVariantList as = b.accounts();
        CHECK(as.size() == 1);
        CHECK(as.value(0).toMap()["name"].toString() == "work");
        CHECK(as.value(0).toMap()["domain"].toString() == "127.0.0.1");
        CHECK(b.currentAccount() == id);

        /* A second account is a second row, not a replacement -- which
           is the whole point of this pane. */
        QVariantMap second = f;
        second["id"] = "lab";
        second["user"] = "1001";
        const qlonglong id2 = b.saveAccount(0, second);
        CHECK(id2 != 0 && id2 != id);
        CHECK(b.accounts().size() == 2);

        /* Editing keeps two accounts, and reads back edited. */
        QVariantMap edit = b.accountCfg(id2);
        CHECK(edit["user"].toString() == "1001");
        edit["user"] = "1002";
        const qlonglong id3 = b.saveAccount(id2, edit);
        CHECK(id3 != 0);
        CHECK(b.accounts().size() == 2);
        CHECK(b.accountCfg(id3)["user"].toString() == "1002");

        b.setCurrentAccount(id3);
        CHECK(b.currentAccount() == id3);

        CHECK(b.removeAccount(id3));
        CHECK(b.accounts().size() == 1);
        /* Removing the selected one must leave a selection, or the
           keypad would have nothing to dial from. */
        CHECK(b.currentAccount() == id);
        CHECK(b.removeAccount(id));
        CHECK(b.accounts().isEmpty());
        CHECK(b.currentAccount() == 0);
    }

    printf("contact pictures, writing\n");
    {
        /* A source picture on disk, because that is what a file
           picker hands over. Deliberately not square: the crop has to
           be the thing that squares it. */
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(dir);
        const QString src = dir + "/probe-source.png";
        QImage art(400, 300, QImage::Format_RGB32);
        art.fill(Qt::darkCyan);
        CHECK(art.save(src, "PNG"));

        CHECK(b.stagePicture(QUrl::fromLocalFile(src).toString()).isEmpty());
        const QVariantMap info = b.stagedPicture();
        CHECK(info["width"].toInt() == 400 && info["height"].toInt() == 300);

        QStringList numbers;
        numbers << "work|sip:1000@pbx.example.lan";
        const qlonglong cid =
            b.saveContact(0, "Picture Probe", "", "", false, numbers);
        CHECK(cid != 0);
        CHECK(b.contactPicture(cid).isEmpty());

        CHECK(b.commitPicture(cid, 50, 0, 300).isEmpty());
        const QString uri = b.contactPicture(cid);
        CHECK(uri.startsWith("data:image/png;base64,"));

        /* Stored at the size the core and both platforms agree on,
           not at whatever the source happened to be. */
        const QByteArray png = QByteArray::fromBase64(
            uri.mid(QString("data:image/png;base64,").size()).toLatin1());
        QImage stored;
        CHECK(stored.loadFromData(png, "PNG"));
        CHECK(stored.width() == 256 && stored.height() == 256);

        CHECK(b.clearPicture(cid).isEmpty());
        CHECK(b.contactPicture(cid).isEmpty());

        /* A file that is not a picture is refused with a sentence,
           not with a crash or an empty avatar. */
        const QString junk = dir + "/probe-source.txt";
        QFile f(junk);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("not a picture");
        f.close();
        CHECK(!b.stagePicture(QUrl::fromLocalFile(junk).toString()).isEmpty());

        CHECK(b.deleteContact(cid));
    }

    printf("audio levels and mute\n");
    {
        b.setLevels(1.5, 0.5, false, false);
        QVariantMap l = b.levels();
        CHECK(qAbs(l["tx"].toDouble() - 1.5) < 0.001);
        CHECK(qAbs(l["rx"].toDouble() - 0.5) < 0.001);
        CHECK(l["txMuted"].toBool() == false);

        /* Muting leaves the stored level alone, so unmuting returns to
           where it was rather than to unity. */
        b.setLevels(1.5, 0.5, true, true);
        l = b.levels();
        CHECK(l["txMuted"].toBool() && l["rxMuted"].toBool());
        CHECK(qAbs(l["tx"].toDouble() - 1.5) < 0.001);

        /* Out of range is refused by the core rather than clamped, and
           the bridge passes that refusal on rather than hiding it. */
        CHECK(!b.setLevels(2.5, 1.0, false, false));
        CHECK(qAbs(b.levels()["tx"].toDouble() - 1.5) < 0.001);

        b.setLevels(1.0, 1.0, false, false);
    }

    printf("codec priority and parameters\n");
    {
        QVariantList cs = b.codecs();
        CHECK(cs.size() >= 2);
        /* The supports_ flags come from the media stack, so the pane
           can grey out a control rather than offer one that does
           nothing. Opus is the only codec here with a settable rate. */
        QString opus, gsm;
        for (const QVariant &v : cs) {
            const QVariantMap m = v.toMap();
            if (m["supportsBitrate"].toBool())
                opus = m["id"].toString();
            else if (gsm.isEmpty())
                gsm = m["id"].toString();
        }
        CHECK(!opus.isEmpty());
        CHECK(!gsm.isEmpty());

        CHECK(b.setCodecParam(opus, "complexity", 5).isEmpty());
        for (const QVariant &v : b.codecs())
            if (v.toMap()["id"].toString() == opus)
                CHECK(v.toMap()["complexity"].toInt() == 5);

        /* -1 is "the codec's own default" and has to survive the round
           trip, or a default becomes a decision nobody made. */
        CHECK(b.setCodecParam(opus, "complexity", -1).isEmpty());
        for (const QVariant &v : b.codecs())
            if (v.toMap()["id"].toString() == opus)
                CHECK(v.toMap()["complexity"].toInt() == -1);

        /* A bitrate on a codec that has none is refused by the core,
           and the sentence reaches the operator. */
        CHECK(!b.setCodecParam(gsm, "bitrate", 32000).isEmpty());
        CHECK(!b.setCodecParam(opus, "complexity", 99).isEmpty());

        /* Order is the offer order, so moving one changes the list. */
        const QString first = b.codecs().value(0).toMap()["id"].toString();
        const QString second = b.codecs().value(1).toMap()["id"].toString();
        CHECK(b.moveCodec(first, 1).isEmpty());
        CHECK(b.codecs().value(0).toMap()["id"].toString() == second);
        CHECK(b.codecs().value(1).toMap()["id"].toString() == first);
        CHECK(b.moveCodec(first, -1).isEmpty());
        CHECK(b.codecs().value(0).toMap()["id"].toString() == first);
        /* Off the end is a no-op, not a reorder. */
        CHECK(b.moveCodec(first, -1).isEmpty());
        CHECK(b.codecs().value(0).toMap()["id"].toString() == first);
    }

    printf("directories, sounds, header names, log order\n");
    {
        const QString home =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

        /* Recording and trace directories. A relative path is refused
           by the core rather than resolved against a working
           directory nobody chose. */
        const QString recDir = home + "/probe-recordings";
        CHECK(b.setRecordingDir(recDir).isEmpty());
        CHECK(b.recordingSettings()["dir"].toString() == recDir);
        CHECK(!b.setRecordingDir("somewhere/relative").isEmpty());

        const QString trDir = home + "/probe-traces";
        CHECK(b.setTraceDir(trDir).isEmpty());
        CHECK(b.recordingSettings()["traceDir"].toString() == trDir);

        /* Importing a sound. A file that is not a WAV is refused: one
           that cannot be played is worse stored than rejected. */
        const QString notWav = home + "/probe-not.wav";
        QFile bad(notWav);
        CHECK(bad.open(QIODevice::WriteOnly));
        bad.write("RIFFnope");
        bad.close();
        CHECK(!b.importSound(0, QUrl::fromLocalFile(notWav).toString())
                   .isEmpty());

        /* The chooser opens where soundspage.cpp's does. */
        CHECK(b.musicFolder()
              == QUrl::fromLocalFile(QStandardPaths::writableLocation(
                     QStandardPaths::MusicLocation)));

        /* Header names the editor completes from. */
        CHECK(b.rememberHeaderName("X-Sp-Probe"));
        CHECK(b.headerNames().contains("X-Sp-Probe"));

        /* Log order, and the three logs that can be emptied. */
        b.setLogOrder(1);
        CHECK(b.logOrder() == 1);
        b.setLogOrder(0);
        CHECK(b.logOrder() == 0);
        CHECK(b.clearLog("sip"));
        CHECK(b.clearLog("history"));
        CHECK(!b.clearLog("nothing-called-this"));

        /* Clearing proved on the one log this test can write to, so
           the assertion is about the clear rather than about the log
           having been empty already. */
        CHECK(b.clearLog("activity"));
        CHECK(b.logAction(1, "probe-activity-entry"));
        bool sawEntry = false;
        for (const QVariant &v : b.activity(50))
            if (v.toMap()["text"].toString().contains("probe-activity-entry"))
                sawEntry = true;
        CHECK(sawEntry);
        CHECK(b.clearLog("activity"));
        bool stillThere = false;
        for (const QVariant &v : b.activity(50))
            if (v.toMap()["text"].toString().contains("probe-activity-entry"))
                stillThere = true;
        CHECK(!stillThere);
    }

    printf("theme preview and overrides\n");
    {
        /* A preview resolves without storing, so the picker can show
           exactly what saving would produce. */
        const QVariantMap before = b.theme();
        const QVariantMap p = b.previewTheme(2, "#FF8800", "", 2);
        CHECK(p["accent"].toString().toUpper() == "#FF8800");
        CHECK(p["background"].toString() != "");
        CHECK(p["dark"].toBool() == false);   /* preset 2 is Light */
        CHECK(b.theme()["accent"].toString() == before["accent"].toString());

        /* An accent that is not #RRGGBB is refused rather than
           quietly ignored. */
        CHECK(b.previewTheme(2, "not-a-colour", "", 2).isEmpty());

        /* Saving an override takes, and reads back. */
        CHECK(b.applyTheme(0, "#22CCAA", "", 3).isEmpty());
        CHECK(b.accent() == QColor(QStringLiteral("#22CCAA")));
        CHECK(b.textScale() == 3);
        CHECK(b.applyTheme(0, "", "", 1).isEmpty());
        /* Cleared, the preset's own accent comes back. */
        CHECK(b.accent() != QColor(QStringLiteral("#22CCAA")));
        CHECK(b.textScale() == 1);
    }

    printf("what the window reflects\n");
    {
        const QVariantMap st = b.settings();
        CHECK(st.contains("sipLog"));
        CHECK(st.contains("sipLogRedact"));
        CHECK(st.contains("sipLogMax"));
        CHECK(st.contains("logLevel"));
        CHECK(st.contains("sipPort"));

        b.setShowCallLog(false);
        CHECK(b.showCallLog() == false);
        b.setShowCallLog(true);
        CHECK(b.showCallLog() == true);
        b.setShowActivityLog(false);
        CHECK(b.showActivityLog() == false);
        b.setShowActivityLog(true);
        CHECK(b.showActivityLog() == true);
    }

    printf("more than one call\n");
    {
        /* No peer here, so this proves the shape and the refusals;
           two live calls are proven against a real peer in the
           integration test. */
        CHECK(b.calls().isEmpty());
        CHECK(b.currentCall() == 0);
        /* A transfer between handles that do not exist must fail
           rather than report success on nothing. */
        CHECK(!b.xferAttended(1, 2));
        b.setCurrentCall(0);
        CHECK(b.currentCall() == 0);
    }

    printf("dialling says why it could not\n");
    {
        /* A refused call went nowhere at all before -- on a laptop
           that looks exactly like a keyboard that did nothing. */
        QVariantMap f;
        f["id"] = "dialprobe";
        f["user"] = "1000";
        f["domain"] = "127.0.0.1";
        f["doRegister"] = false;
        f["persist"] = false;
        const qlonglong acc = b.saveAccount(0, f);
        CHECK(acc != 0);

        /* A bare extension is completed with the account's domain,
           which is the core's rule, not the window's. */
        CHECK(!b.dial(""));
        CHECK(!b.lastError().isEmpty());

        CHECK(b.removeAccount(acc));
    }

    printf("who is calling\n");
    {
        /* An incoming call shows a URI unless somebody looks the
           number up, and the lookup rule is the core's: exact, on a
           normalised form. */
        QStringList numbers;
        numbers << "work|sip:6046@pbx.example.lan";
        const qlonglong cid =
            b.saveContact(0, "Sylvie Tremblay", "Acme", "", false, numbers);
        CHECK(cid != 0);

        const QVariantMap who = b.whoIs("sip:6046@pbx.example.lan");
        CHECK(who["name"].toString() == "Sylvie Tremblay");
        CHECK(who["id"].toLongLong() == cid);

        /* No suffix matching: 4185 is a desk and 14185551234 is not
           it, which is the core's own warning. */
        CHECK(b.whoIs("sip:9999@pbx.example.lan").isEmpty());
        CHECK(b.whoIs("").isEmpty());

        CHECK(b.deleteContact(cid));
    }

    printf("the network changed\n");
    {
        /* Bridged nowhere and implemented nowhere until now. On a
           started core it rebuilds transports and re-registers. */
        CHECK(b.networkChanged());
    }

    printf("where the password lives\n");
    {
        /* The whole point: the account persists, the password does
           not reach the file the core writes. */
        printf("  (store: %s)\n",
               b.credentialStore().toUtf8().constData());

        QVariantMap f;
        f["id"] = "secretprobe";
        f["user"] = "1000";
        f["domain"] = "127.0.0.1";
        f["password"] = "probe-password-not-in-the-file";
        f["doRegister"] = true;
        f["persist"] = true;
        const qlonglong acc = b.saveAccount(0, f);
        CHECK(acc != 0);

        /* Read back through the same path the editor uses. With a
           store it comes from the store; without one it comes from
           the core, which is the fallback the window names. */
        const QVariantMap back = b.accountCfg(acc);
        CHECK(back["password"].toString()
              == "probe-password-not-in-the-file");

        if (b.credentialStoreAvailable()) {
            const QString home = QStandardPaths::writableLocation(
                QStandardPaths::AppConfigLocation);
            QFile cfgFile(home + "/comlink.yaml");
            CHECK(cfgFile.open(QIODevice::ReadOnly));
            const QByteArray written = cfgFile.readAll();
            cfgFile.close();
            CHECK(!written.contains("probe-password-not-in-the-file"));
            CHECK(written.contains("secretprobe"));
        }

        /* A rename must take the stored password with it, and leave
           nothing behind under the old name: a SIP credential filed
           under a name nothing refers to is one nobody will clear. */
        if (b.credentialStoreAvailable()) {
            QVariantMap renamed = b.accountCfg(acc);
            renamed["id"] = "secretprobe2";
            const qlonglong acc2 = b.saveAccount(acc, renamed);
            CHECK(acc2 != 0);
            CHECK(b.accountCfg(acc2)["password"].toString()
                  == "probe-password-not-in-the-file");
            CHECK(b.accountCfg(acc2)["passwordKnown"].toBool());
            /* Read from the store itself, not through the account:
               an account read back would find nothing either way,
               because saving one with an empty password clears the
               entry as a matter of course. What is being checked is
               that the entry under the OLD name is already gone. */
            CHECK(SecretStore::load("account/secretprobe").isEmpty());
            CHECK(SecretStore::load("account/secretprobe2")
                  == "probe-password-not-in-the-file");
            CHECK(b.removeAccount(acc2));
            CHECK(SecretStore::load("account/secretprobe2").isEmpty());
        } else {
            CHECK(b.removeAccount(acc));
        }
    }

    printf("initials from the core\n");
    CHECK(b.initialsOf("Sylvie Tremblay") == "ST");
    CHECK(b.initialsOf("Repartition") == "R");

    printf("\n%s\n", fails == 0 ? "all bridge checks passed"
                                : "BRIDGE CHECKS FAILED");
    return fails == 0 ? 0 : 1;
}
