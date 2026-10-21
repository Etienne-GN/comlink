#include <cstdlib>

#include <atomic>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QPermissions>
#include <QQmlApplicationEngine>
#include "corebridge.h"
#include "modelfetcher.h"
#include "comlink/core.h"
#include <QQuickWindow>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QStringList>
#include <QTimer>

/* QML reports a missing property, a type error or a failed binding as
   a warning and carries on, so a pane that half-loaded looks like a
   pane that rendered. COMLINK_QUICK_STRICT counts them and makes the
   process exit non-zero, which is what lets a script assert "this
   pane loaded cleanly" instead of a human deciding from a picture.
 *
 * Only QML's own warnings. Counting every Qt warning made the package
 * build fail on "Detected locale C ... switched to C.UTF-8", which is
 * a fact about the container and not about a pane -- a test that goes
 * red for the wrong reason is a test nobody will trust the next
 * time.
 *
 * Not every QML warning names its file, though. A builtin such as
 * Qt.lighter() warns in the default category with no context at all,
 * and the engine's type checks arrive as qt.qml.*: both went past
 * this filter while every button in the window was drawn black. So
 * the default category counts too, less that one locale notice. */
static std::atomic<int> g_qmlWarnings{0};

static bool fromQml(const QMessageLogContext &ctx, const QString &msg) {
    const QString category = QString::fromUtf8(ctx.category ? ctx.category
                                                            : "");
    const QString file = QString::fromUtf8(ctx.file ? ctx.file : "");
    if (category == QLatin1String("default"))
        return !msg.startsWith(QLatin1String("Detected locale"));
    return category == QLatin1String("qml")
        || category == QLatin1String("js")
        || category.startsWith(QLatin1String("qt.qml"))
        || file.endsWith(QLatin1String(".qml"))
        || msg.contains(QLatin1String(".qml:"))
        || msg.contains(QLatin1String("qrc:"));
}

static void countWarnings(QtMsgType type, const QMessageLogContext &ctx,
                          const QString &msg) {
    if ((type == QtWarningMsg || type == QtCriticalMsg
         || type == QtFatalMsg)
        && fromQml(ctx, msg))
        g_qmlWarnings.fetch_add(1);
    const QByteArray line =
        (msg + QLatin1Char('\n')).toLocal8Bit();
    fwrite(line.constData(), 1, line.size(), stderr);
}

/* The offscreen platform has no GPU to render through, and the
   default scene graph backend fails there rather than falling back.
   Software rendering is what makes an offscreen grab possible at all,
   which is what the probe below needs -- the same trick the Widgets
   frontend gets for free. */
static void useSoftwareIfOffscreen() {
    const char *plat = std::getenv("QT_QPA_PLATFORM");
    if (plat && QByteArray(plat).startsWith("offscreen"))
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
}

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include "comlink/core.h"
#include <android/log.h>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

/* stdout and stderr, into logcat. The core reports through stderr and
   pjsip through stdout, and on Android both go nowhere: a call with no
   audio left no trace on the handset. One thread reads both through a
   pipe and writes each line out under the "comlink" tag. */
static void *logcatPump(void *arg) {
    FILE *in = fdopen((int)(intptr_t)arg, "r");
    if (!in)
        return nullptr;
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n > 0)
            __android_log_write(ANDROID_LOG_INFO, "comlink", line);
    }
    fclose(in);
    return nullptr;
}

static void stdioToLogcat() {
    int fds[2];
    if (pipe(fds) != 0)
        return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    pthread_t t;
    if (pthread_create(&t, nullptr, logcatPump, (void *)(intptr_t)fds[0]) == 0)
        pthread_detach(t);
    else
        close(fds[0]);
}

/* The core opens sound files by path, and an APK has no paths -- its
   resources live inside the package. They are unpacked once, beside
   the configuration, and COMLINK_SOUND_DIR points the core at them.
 *
   Copied rather than mapped because pjmedia's WAV player takes a
   filename, and rewriting that to take a stream would be a change to
   the media path for the sake of a packaging detail. */
static void unpackSounds() {
    const QString dest =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/sounds");
    QDirIterator it(QStringLiteral(":/sounds"), QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        if (!QFileInfo(src).isFile())
            continue;
        const QString rel = src.mid(QStringLiteral(":/sounds/").size());
        const QString out = dest + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(out).absolutePath());
        /* Overwritten every start: an upgrade that ships a new sound
           should not be waiting behind a copy from the old one. */
        QFile::remove(out);
        QFile::copy(src, out);
        QFile::setPermissions(out, QFileDevice::ReadOwner
                                       | QFileDevice::WriteOwner);
    }
    qputenv("COMLINK_SOUND_DIR", dest.toUtf8());
}
#endif

#ifndef Q_OS_ANDROID
static QString defaultConfigPath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("comlink.yaml"));
}
#endif

int main(int argc, char *argv[]) {
#ifndef Q_OS_ANDROID
    /* Answered before anything else, and before QGuiApplication
       exists: asking what a binary is must not require a display.
       Constructing the application first aborts over ssh and in a
       container, where there is no platform plugin to load. sp_build
       and qVersion both answer without one. */
    for (int i = 1; i < argc; i++) {
        if (qstrcmp(argv[i], "--version") != 0)
            continue;
        const sp_build_info *b = sp_build();
        fprintf(stdout, "comlink %s (%s, configured %s)\npjproject %s\nQt %s\n",
                b->version, b->git_commit, b->configured_date,
                b->pjproject_version, qVersion());
        return 0;
    }
#endif
#ifdef Q_OS_ANDROID
    /* The first call the process makes, before the core is created.
       pjlib's Android build reaches Java through JNI for two things
       it has no native path for: a dialog tag comes from
       java.util.UUID.randomUUID(), and the capture device is
       android.media.AudioRecord. Without the VM the first is an
       empty string, which trips an assertion inside pjsip the moment
       a dialog is created -- so the first call placed would have
       aborted -- and the second is a null JNIEnv dereferenced in
       pjmedia.

       The Java frontend did this in its JNI_OnLoad. This frontend
       has no JNI_OnLoad of its own, so nothing did it at all. */
    stdioToLogcat();
    sp_core_set_jvm(QJniEnvironment::javaVM());
    unpackSounds();
#endif
    const bool strict = qEnvironmentVariableIntValue("COMLINK_QUICK_STRICT");
    if (strict)
        qInstallMessageHandler(countWarnings);
    useSoftwareIfOffscreen();
    QGuiApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    /* The desktop's faces, so text takes the room it takes there. A
       theme that names a family still overrides this. */
    for (const char *f : {":/fonts/DejaVuSans.ttf", ":/fonts/DejaVuSans-Bold.ttf",
                          ":/fonts/DejaVuSansMono.ttf"})
        QFontDatabase::addApplicationFont(QString::fromLatin1(f));
    {
        QFont f(QStringLiteral("DejaVu Sans"));
        app.setFont(f);
        QFont::insertSubstitution(QStringLiteral("monospace"),
                                  QStringLiteral("DejaVu Sans Mono"));
    }
#endif

#ifndef Q_OS_ANDROID
    app.setApplicationName(QStringLiteral("comlink"));
    app.setApplicationDisplayName(QStringLiteral("Comlink"));
    app.setApplicationVersion(QString::fromUtf8(sp_build()->version));
    /* The icon theme first, so a user's theme can override it, then
       the embedded copy. Without the fallback the window and the task
       switcher show a generic placeholder whenever the application
       runs from the build tree or before the package is installed. */
    app.setDesktopFileName(QStringLiteral("comlink"));
    QIcon icon = QIcon::fromTheme(QStringLiteral("comlink"));
    if (icon.isNull())
        icon = QIcon(QStringLiteral(":/comlink-256.png"));
    app.setWindowIcon(icon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "SIP endpoint and diagnostic instrument for telephony technicians"));
    parser.addHelpOption();
    QCommandLineOption configOpt(
        QStringList{QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Configuration file (default: %1)")
            .arg(defaultConfigPath()),
        QStringLiteral("path"));
    parser.addOption(configOpt);
    parser.process(app);
    if (parser.isSet(configOpt))
        CoreBridge::setConfigPath(parser.value(configOpt));
#endif

    /* Declared in the manifest and asked for here: on API 23 and up
       the declaration alone grants nothing, and a permission dialog
       in the middle of answering a call is a lost call. Qt answers
       Undetermined on platforms with no such concept, which is the
       desktop, so this is a no-op there. */
    QMicrophonePermission mic;
    if (qApp->checkPermission(mic) == Qt::PermissionStatus::Undetermined)
        qApp->requestPermission(mic, [](const QPermission &) {});

    QQmlApplicationEngine engine;
    engine.loadFromModule("comlink", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    /* The first model fetched becomes the one in use when none is set,
       wherever the operator is when it lands: by then the Settings
       page that started it may be long closed. Connected before any
       page can connect, so a page reloading on finished sees the
       result. */
    auto *bridge = engine.singletonInstance<CoreBridge *>("comlink", "CoreBridge");
    auto *fetcher = engine.singletonInstance<ModelFetcher *>("comlink",
                                                             "ModelFetcher");
    if (bridge && fetcher)
        QObject::connect(fetcher, &ModelFetcher::finished, bridge,
                         [bridge](const QString &, const QString &path) {
            if (!bridge->transcriptModel().isEmpty())
                return;
            const QString why = bridge->setTranscriptModel(path);
            if (!why.isEmpty())
                bridge->note(QStringLiteral("the downloaded model could "
                                            "not be used: %1").arg(why));
        });

    /* COMLINK_QUICK_KEYS types into the window before the shot is
       taken, so the keyboard path can be exercised without a human at
       one. Comma-separated: a single character is that key, and
       Return, Escape, Backspace and Tab are named. A "Ctrl+" prefix
       adds the modifier; "Shift+Tab" is sent as the Backtab Qt makes
       of it.

       Delivered as real key events through the window, not by calling
       the handler: what is being checked is that the key reaches the
       handler at all, past whatever has focus. */
    const char *keys = std::getenv("COMLINK_QUICK_KEYS");
    if (keys && *keys) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QStringList specs =
            QString::fromUtf8(keys).split(QLatin1Char(','),
                                          Qt::SkipEmptyParts);
        QTimer::singleShot(300, &app, [win, specs]() {
            /* Into the dialog when one is open, for the same reason the
               shot is taken of it: that is where the keys are meant. */
            QQuickWindow *to = win;
            if (qEnvironmentVariableIsSet("COMLINK_QUICK_DIALOG"))
                for (QWindow *w : QGuiApplication::topLevelWindows())
                    if (w != win && w->isVisible())
                        if (auto *q = qobject_cast<QQuickWindow *>(w))
                            to = q;
            for (QString spec : specs) {
                Qt::KeyboardModifiers mods = Qt::NoModifier;
                if (spec.startsWith(QStringLiteral("Ctrl+"))) {
                    mods |= Qt::ControlModifier;
                    spec = spec.mid(5);
                }
                int key = 0;
                QString text;
                if (spec == QStringLiteral("Return")) {
                    key = Qt::Key_Return;
                } else if (spec == QStringLiteral("Escape")) {
                    key = Qt::Key_Escape;
                } else if (spec == QStringLiteral("Backspace")) {
                    key = Qt::Key_Backspace;
                } else if (spec == QStringLiteral("Tab")) {
                    key = Qt::Key_Tab;
                } else if (spec == QStringLiteral("Shift+Tab")) {
                    key = Qt::Key_Backtab;
                    mods |= Qt::ShiftModifier;
                } else if (spec.size() == 1) {
                    key = spec.at(0).toUpper().unicode();
                    text = spec;
                } else {
                    continue;
                }
                QKeyEvent press(QEvent::KeyPress, key, mods, text);
                QKeyEvent release(QEvent::KeyRelease, key, mods, text);
                QCoreApplication::sendEvent(to, &press);
                QCoreApplication::sendEvent(to, &release);
            }
            fprintf(stderr, "PROBE quick keys sent=%lld\n",
                    (long long)specs.size());
        });
    }

    /* COMLINK_QUICK_SHOT writes the window to a PNG and exits, so the
       two platforms can be compared without a human holding a phone
       next to a monitor. The Widgets frontend has the same probe. */
    const char *shot = std::getenv("COMLINK_QUICK_SHOT");
    if (shot && *shot) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!win)
            return 1;
        const QString path = QString::fromUtf8(shot);
        QTimer::singleShot(700, &app, [win, path]() {
            /* A secondary window opened by COMLINK_QUICK_OVERLAY is its
               own top-level on the desktop, and it is the one wanted:
               the dialog, not the window behind it. */
            QQuickWindow *pick = win;
            if (qEnvironmentVariableIsSet("COMLINK_QUICK_DIALOG"))
                for (QWindow *w : QGuiApplication::topLevelWindows())
                    if (w != win && w->isVisible())
                        if (auto *q = qobject_cast<QQuickWindow *>(w))
                            pick = q;
            /* COMLINK_QUICK_SHOT_SIZE=WxH: the window at another size,
               so a layout that depends on width -- Settings when
               narrow -- can be seen without a handset. */
            const QStringList size =
                QString::fromUtf8(qgetenv("COMLINK_QUICK_SHOT_SIZE"))
                    .split(QLatin1Char('x'));
            int delay = 0;
            if (size.size() == 2) {
                pick->resize(size.at(0).toInt(), size.at(1).toInt());
                delay = 400;
            }
            QTimer::singleShot(delay, pick, [pick, path]() {
                const bool ok = pick->grabWindow().save(path);
                fprintf(stderr, "PROBE quick window written=%d to %s\n",
                        ok ? 1 : 0, path.toUtf8().constData());
                QGuiApplication::quit();
            });
        });
    }
    const int rc = app.exec();
    if (strict && g_qmlWarnings.load() > 0) {
        fprintf(stderr, "PROBE qml warnings=%d\n", g_qmlWarnings.load());
        return 2;
    }
    return rc;
}
