#ifndef COMLINK_COREBRIDGE_H
#define COMLINK_COREBRIDGE_H

#include <QColor>
#include <QImage>
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif
#include <QtQml/qqmlregistration.h>

#include "comlink/core.h"

/* The only C++ in the QML frontend.
 *
 * It marshals and it does not decide. Anything here that looks like a
 * product rule belongs in the core instead -- not because the other
 * frontend would have to repeat it, but because there is no other
 * frontend and this file is where that discipline would quietly
 * lapse.
 *
 * Threading: sp_event_cb arrives on the core's own dispatch thread.
 * Every one is posted to this object's thread with a queued
 * invocation before anything QML can see is touched. */
class CoreBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /* Which pane to open on. The Qt window has COMLINK_PANE_PROBE for
       the same reason: a screenshot of a pane needs no human to tap
       the strip. */
    Q_PROPERTY(int startPane READ startPane CONSTANT)
    /* Which overlay to open on, by name, for the same reason: an
       overlay nothing can open from the outside is an overlay no
       screenshot and no smoke test ever sees. */
    Q_PROPERTY(QString startOverlay READ startOverlay CONSTANT)
    /* What to put in the prompt, and whether to dial it, so a
       screenshot of a call in progress needs no human typing a URI. */
    Q_PROPERTY(QString startDial READ startDial CONSTANT)
    /* A screenshot probe is running, so the window reports its
       geometry; an operator's terminal is not the place for it. */
    Q_PROPERTY(bool probing READ probing CONSTANT)

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString pjproject READ pjproject CONSTANT)
    Q_PROPERTY(QString commit READ commit CONSTANT)
    /* What the About dialog shows and nothing else needs: the running
       Qt version, and the three paths a ticket usually wants. */
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(QString databasePath READ databasePath CONSTANT)
    Q_PROPERTY(QString soundDir READ soundDir CONSTANT)

    /* The theme, resolved by the core. Every colour in every .qml file
       comes from these and none is written in a component. QColor and
       not the core's hex string: QML reads .r/.g/.b off these, which a
       string does not have, and Qt 6.11 draws the NaN that follows as
       black. */
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
    Q_PROPERTY(QColor surface READ surface NOTIFY themeChanged)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY themeChanged)
    Q_PROPERTY(QColor muted READ muted NOTIFY themeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY themeChanged)
    Q_PROPERTY(QColor go READ go NOTIFY themeChanged)
    Q_PROPERTY(QColor stop READ stop NOTIFY themeChanged)
    Q_PROPERTY(QColor txTrace READ txTrace NOTIFY themeChanged)
    Q_PROPERTY(QColor rxTrace READ rxTrace NOTIFY themeChanged)
    /* The application font's size in points, which every other size
       in the Qt window is a multiple of. */
    Q_PROPERTY(qreal basePt READ basePt NOTIFY themeChanged)
    /* The preset's mark behind the call, "" for none. */
    Q_PROPERTY(QString emblem READ emblem NOTIFY themeChanged)

    /* What the window shows about itself. */
    Q_PROPERTY(bool started READ started NOTIFY startedChanged)
    Q_PROPERTY(QString accountLabel READ accountLabel NOTIFY accountChanged)
    Q_PROPERTY(bool registered READ registered NOTIFY accountChanged)
    Q_PROPERTY(QString registrationDetail READ registrationDetail
               NOTIFY accountChanged)
    /* Why the last thing that could fail did. A property rather than
       a method because the window binds to it: a method call in a
       binding is evaluated once and never again, so a refusal that
       arrived later never reached the screen. */
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    /* The current call, as the call display needs it. */
    Q_PROPERTY(bool inCall READ inCall NOTIFY callChanged)
    Q_PROPERTY(int callState READ callState NOTIFY callChanged)
    Q_PROPERTY(QString callTitle READ callTitle NOTIFY callChanged)
    Q_PROPERTY(QString remoteLabel READ remoteLabel NOTIFY callChanged)
    Q_PROPERTY(QString remoteUri READ remoteUri NOTIFY callChanged)
    Q_PROPERTY(QString codec READ codec NOTIFY callChanged)
    Q_PROPERTY(QString clock READ clock NOTIFY callChanged)
    Q_PROPERTY(QString qualityLine READ qualityLine NOTIFY qualityChanged)
    Q_PROPERTY(qreal qualityMos READ qualityMos NOTIFY qualityChanged)
    Q_PROPERTY(bool ringing READ ringing NOTIFY callChanged)
    Q_PROPERTY(QString remoteUser READ remoteUser NOTIFY callChanged)
    /* The book's name for the far end, empty when the book has none:
       the display shows a name line only when there is a name. */
    Q_PROPERTY(QString contactName READ contactName NOTIFY callChanged)
    /* Why nothing registered, when the transport itself failed. */
    Q_PROPERTY(QString transportError READ transportError
               NOTIFY accountChanged)
    Q_PROPERTY(bool onHold READ onHold NOTIFY callChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY callChanged)

public:
    /* Where the configuration lives, when the command line said so.
       Set before the engine builds the singleton; empty means the
       platform's own location, which is what a handset always
       uses. */
    static void setConfigPath(const QString &path);

    explicit CoreBridge(QObject *parent = nullptr);
    ~CoreBridge() override;

    int     startPane() const;
    QString startOverlay() const;
    QString startDial() const;
    bool    probing() const {
        return qEnvironmentVariableIsSet("COMLINK_QUICK_SHOT");
    }
    QString version() const;
    QString pjproject() const;
    QString commit() const;
    QString qtVersion() const;
    QString configPath() const { return m_cfgPath; }
    QString databasePath() const;
    QString soundDir() const;

    QColor  accent() const;
    QColor  background() const;
    QColor  surface() const;
    QColor  foreground() const;
    QColor  muted() const;
    bool    dark() const;
    QColor  go() const;
    QColor  stop() const;
    QColor  txTrace() const;
    QColor  rxTrace() const;
    qreal   basePt() const;
    QString emblem() const {
        return m_haveTheme && m_theme.emblem ? QString::fromUtf8(m_theme.emblem)
                                             : QString();
    }

    bool    started() const { return m_started; }
    QString accountLabel() const;
    bool    registered() const { return m_registered; }
    QString registrationDetail() const { return m_regDetail; }
    QString lastError() const { return m_lastError; }

    bool    inCall() const { return m_call != 0; }
    int     callState() const { return m_callState; }
    QString callTitle() const;
    QString remoteLabel() const { return m_remoteLabel; }
    QString remoteUri() const { return m_remoteUri; }
    QString codec() const { return m_codec; }
    QString clock() const;
    QString qualityLine() const { return m_quality; }
    qreal   qualityMos() const { return m_mos; }
    bool    ringing() const;
    QString remoteUser() const { return m_remoteUser; }
    QString contactName() const { return m_contactName; }
    QString transportError() const { return m_transportError; }
    bool    onHold() const { return m_onHold; }
    bool    recording() const { return m_recording; }

    /* The core's rule, not a QML one: "Sylvie Tremblay" is ST and
       "Repartition" is R. */
    Q_INVOKABLE QString initialsOf(const QString &name) const;
    /* The activity pane: the window's own account of what it did,
       one timestamped line each, as the Qt window keeps it. */
    Q_INVOKABLE void note(const QString &text);
    Q_INVOKABLE QStringList windowLog() const;

    Q_INVOKABLE bool start();
    /* Accounts, plural. The rows the settings pane repeats over and
     * the picker in the status row reads; "current" is the one a call
     * is placed from.
     *
     * A form is the same map in both directions: id (the short name),
     * user, domain, authUser, password, registrar, proxy, stun,
     * transport, srtp, expires, doRegister, persist. Nothing about it
     * is judged here -- validateAccount asks the core, so both
     * platforms refuse the same account with the same sentence. */
    Q_INVOKABLE QVariantList accounts() const;
    Q_INVOKABLE qlonglong currentAccount() const;
    Q_INVOKABLE void setCurrentAccount(qlonglong id);
    Q_INVOKABLE QVariantMap accountCfg(qlonglong id) const;
    Q_INVOKABLE QString validateAccount(const QVariantMap &form) const;
    /* id 0 adds; anything else replaces that account in place. Returns
       the account's handle afterwards, which the core renews on every
       update, or 0. */
    Q_INVOKABLE qlonglong saveAccount(qlonglong id, const QVariantMap &form);
    Q_INVOKABLE bool removeAccount(qlonglong id);
    Q_INVOKABLE bool setRegistration(qlonglong id, bool on);

    /* Where a SIP password is kept, and whether anywhere is. Shown in
       the settings pane, because a silent fall back to the core's
       cleartext configuration would be the worst of the three
       outcomes. */
    Q_INVOKABLE bool credentialStoreAvailable() const;
    Q_INVOKABLE QString credentialStore() const;
    Q_INVOKABLE bool dial(const QString &uri);
    Q_INVOKABLE void answer();
    Q_INVOKABLE void hangup(int code);
    Q_INVOKABLE void dtmf(const QString &digits);
    /* A digit, wherever it came from: the keypad, or the keyboard.
       It goes out as DTMF once the call is confirmed and into the
       number being dialled otherwise -- which is decided here so the
       keypad and the keyboard cannot disagree. Returns the digit when
       the caller should append it to the prompt, empty when it was
       sent. */
    Q_INVOKABLE QString enterDigit(const QString &digit);
    Q_INVOKABLE void setHold(bool hold);
    Q_INVOKABLE bool transfer(const QString &uri);

    /* Every live call, not just the one on the display. A second call
       is how a transfer is set up, and how a technician holds one
       party while asking another -- the desktop has had a call list
       since it had a call. "current" is the one the display shows.
     *
     * xferAttended connects two calls to each other and drops out:
     * a and b must both be live, and a is the one being transferred. */
    Q_INVOKABLE QVariantList calls() const;
    Q_INVOKABLE qlonglong currentCall() const;
    Q_INVOKABLE void setCurrentCall(qlonglong id);
    Q_INVOKABLE bool xferAttended(qlonglong a, qlonglong b);
    /* Where a recording lands and in what form belong to the core,
       so this asks it to start one rather than naming a file. */
    Q_INVOKABLE QString toggleRecording();

    /* tx and rx, 0..255, or -1 when there is no media yet -- which is
       a normal moment in a call rather than a fault. */
    Q_INVOKABLE int txLevel() const { return m_tx; }
    Q_INVOKABLE int rxLevel() const { return m_rx; }
    Q_INVOKABLE void pollSignal();
    Q_INVOKABLE void refreshCall();

    /* The panes. Each returns rows the QML repeats over; the core
       owns the shape and the order, so a second frontend could not
       sort them differently even if there were one. */
    Q_INVOKABLE QVariantList contacts(const QString &search) const;
    Q_INVOKABLE QVariantList history(int limit) const;
    Q_INVOKABLE QVariantList activity(int limit) const;
    /* The SIP trace the Logs window shows. call_id empty means every
       call; the core redacts before it hands anything over. */
    Q_INVOKABLE QVariantList siplog(const QString &callId, int limit) const;
    Q_INVOKABLE QString sipMessageAt(const QString &callId, int index) const;
    /* 0 newest first, 1 oldest first. Oldest first is what you want
       when you are looking for the first sign of a fault. */
    Q_INVOKABLE int logOrder() const;
    Q_INVOKABLE void setLogOrder(int order);
    /* "sip", "history" or "activity". */
    Q_INVOKABLE bool clearLog(const QString &which);
    /* The frontend's own notable actions, in the same log as the
       core's, so the activity pane is one account of what happened
       rather than two. */
    Q_INVOKABLE bool logAction(int level, const QString &text);
    Q_INVOKABLE QVariantMap quality() const;
    /* The Quality dialog's full table, for an arbitrary call rather
       than only the current one: its own call picker must not reach
       into setCurrentCall and move what the main window is showing.
       "exists" is false once the call's handle has been retired
       (sp_call_get_quality failing outright), distinct from "exists
       but valid is false" (the stream is up but unsampled yet). */
    Q_INVOKABLE QVariantMap qualityDetail(qlonglong callId) const;

    /* The settings the Qt window has pages for. The core owns every
       list and every default; these hand them over and hand choices
       back. */
    Q_INVOKABLE QVariantList themePresets() const;
    Q_INVOKABLE int currentPreset() const;
    Q_INVOKABLE void setPreset(int preset);
    /* The whole theme as the core resolved it, and the same map for a
       candidate that is not stored -- which is what makes a preview
       honest: it shows exactly what saving would produce.
     *
     * An empty accent or font means "the preset's own". An accent
     * that is not #RRGGBB comes back as an empty map rather than as
       a silently ignored setting. */
    Q_INVOKABLE QVariantMap theme() const;
    Q_INVOKABLE QVariantMap previewTheme(int preset, const QString &accent,
                                         const QString &font, int scale) const;
    Q_INVOKABLE QString applyTheme(int preset, const QString &accent,
                                   const QString &font, int scale);
    Q_INVOKABLE int textScale() const;

    /* What the configuration says, for the pane that shows it. Read
       only: changing these means editing the file, which the core
       owns. */
    Q_INVOKABLE QVariantMap settings() const;
    Q_INVOKABLE bool showCallLog() const;
    Q_INVOKABLE void setShowCallLog(bool on);
    Q_INVOKABLE bool showActivityLog() const;
    Q_INVOKABLE void setShowActivityLog(bool on);
    Q_INVOKABLE QVariantList codecs() const;
    /* Offered highest first; the list's order is the offer order and
       the core turns it into priorities. Changes take effect on the
       next call -- a codec's parameters are fixed once its stream
       exists.
     *
     * key is one of bitrate, complexity, loss, ptime, cbr; -1 is the
     * codec's own default throughout, which is why these take an int
     * rather than a variant. Each returns an empty string on success
       and the core's refusal otherwise. */
    Q_INVOKABLE QString setCodecEnabled(const QString &id, bool on);
    Q_INVOKABLE QString setCodecParam(const QString &id, const QString &key,
                                      int value);
    Q_INVOKABLE QString moveCodec(const QString &id, int delta);
    /* The codecs page's own Apply: the whole ordered, edited list at
       once, the same one call sp_media_set_codecs always was. Each
       row is a map with id, enabled, bitrate, complexity, loss,
       ptime, cbr; a row whose id is no longer known is dropped. */
    Q_INVOKABLE QString applyCodecList(const QVariantList &rows);
    Q_INVOKABLE QVariantList modules() const;
    /* Empty on success, otherwise what went wrong: the modules page
       reverts its checkbox on a failure rather than trusting the
       click. */
    Q_INVOKABLE QString setModule(const QString &id, bool on);
    /* A call's transcript, one row per turn of speech:
       {startMs, endMs, local, speaker, text}. */
    Q_INVOKABLE QVariantList transcript(qlonglong historyId) const;
    /* The same, as "mm:ss  Speaker: text" lines, for the clipboard. */
    Q_INVOKABLE QString transcriptText(qlonglong historyId) const;
    /* Whether this build can transcribe at all. */
    Q_INVOKABLE bool transcriptAvailable() const;
    /* The speech model in use, "" for none. */
    Q_INVOKABLE QString transcriptModel() const;
    /* Empty on success, otherwise what went wrong. "" clears it. */
    Q_INVOKABLE QString setTranscriptModel(const QString &path);
    /* The microphone and the speaker: a level each, 0.0 to 2.0, and a
       mute each. On the window rather than behind a settings page --
       a mute you have to navigate to is not a mute. */
    Q_INVOKABLE QVariantMap levels() const;
    Q_INVOKABLE bool setLevels(qreal tx, qreal rx, bool txMuted,
                               bool rxMuted);

    Q_INVOKABLE QVariantMap dsp() const;
    Q_INVOKABLE void setDsp(const QString &key, bool on);
    /* The echo canceller's history, in milliseconds; the only DSP
       setting that is a number rather than a flag, so it is its own
       call rather than a third argument every other caller would have
       to pass as ignored. */
    Q_INVOKABLE void setEchoTailMs(int ms);
    /* Record every answered call without Record being pressed.
       Empty on success, otherwise why not. */
    Q_INVOKABLE QString setRecordingAuto(bool on);
    Q_INVOKABLE QVariantMap recordingSettings() const;
    /* Empty on success, otherwise what went wrong: the recording page
       reverts the format combo on a failure rather than trusting the
       click. */
    Q_INVOKABLE QString setRecordingFormat(int fmt);
    /* Absolute paths only: the core refuses a relative one rather
       than resolving it against a working directory nobody chose.
       Each returns an empty string on success. */
    Q_INVOKABLE QString setRecordingDir(const QString &dir);
    Q_INVOKABLE QString setTraceDir(const QString &dir);

    /* Playback of a recording. A call record carries its own path, so
       nothing here enumerates a directory: the recordings a call
       produced are reached through the call. */
    Q_INVOKABLE bool playbackStart(const QString &path);
    Q_INVOKABLE void playbackPause();
    Q_INVOKABLE void playbackResume();
    Q_INVOKABLE void playbackStop();
    Q_INVOKABLE void playbackSeek(int positionMs);
    Q_INVOKABLE void playbackSpeed(qreal speed);
    Q_INVOKABLE QVariantMap playback() const;
    /* Empty on success, otherwise what went wrong, already logged. */
    Q_INVOKABLE QString saveRecordingCopy(const QString &src,
                                          const QString &destUrl);

    /* The contact editor's side of the book. Numbers arrive as a
       list of "label|uri" so one call can save a contact whole --
       the core replaces the set rather than merging, so a partial
       save would silently drop a number. */
    Q_INVOKABLE QVariantMap contact(qlonglong id) const;
    Q_INVOKABLE qlonglong saveContact(qlonglong id, const QString &name,
                                      const QString &company,
                                      const QString &note, bool favourite,
                                      const QStringList &numbers);
    Q_INVOKABLE bool deleteContact(qlonglong id);
    /* Who is this? An empty map when nobody. The matching rule is the
       core's -- exact, on a normalised form -- so an incoming call
       shows the same name on both platforms or shows the URI on
       both. */
    Q_INVOKABLE QVariantMap whoIs(const QString &uri) const;

    /* Choosing a picture, in three steps, because the picture never
       has to cross the bridge as pixels: QML shows the file itself
       and sends back the square it settled on, in the source's own
       coordinates.
     *
     * The guards are here rather than in the core because they are
     * guards on decoding, and the core has no image decoder -- what
     * the core does enforce, and enforces whatever this file does, is
     * that a stored picture is PNG and under 512 KB.
     *
     * Each returns an empty string on success and a sentence
     * otherwise, so the overlay has something to show. */
    Q_INVOKABLE QString stagePicture(const QString &url);
    Q_INVOKABLE QVariantMap stagedPicture() const;
    Q_INVOKABLE QString commitPicture(qlonglong contactId, int x, int y,
                                      int edge);
    Q_INVOKABLE QString clearPicture(qlonglong contactId);
    /* The crop encoded but not written anywhere -- a data: URI an
       editor can preview locally, the same way contacteditor.cpp
       holds m_pictureBytes and only reaches the core on Save. Empty
       on a bad crop, same guards as commitPicture. */
    Q_INVOKABLE QString stagedPreview(int x, int y, int edge) const;
    /* What the file chooser should offer, asked of Qt rather than
       hand-copied into a QML filter string that would go stale --
       picturedialog.cpp's own acceptedFilterForProbe(). */
    Q_INVOKABLE QStringList pictureFormats() const;

    /* Audio devices and the three sounds. The core enumerates and
       recommends; a frontend that filtered the list itself would
       disagree with the other platform about which card is usable. */
    Q_INVOKABLE QVariantList audioDevices() const;
    Q_INVOKABLE QVariantMap audioDevicesInUse() const;
    /* Empty on success, otherwise what went wrong -- switching devices
       is deliberate and staged behind an Apply button, so the page has
       to be able to say why it did not take. */
    Q_INVOKABLE QString setAudioDevices(const QString &capture,
                                        const QString &playback);
    Q_INVOKABLE QVariantList soundChoices(int slot) const;
    Q_INVOKABLE QString soundInUse(int slot) const;
    /* Empty on success, otherwise what went wrong: the sounds page
       reverts the combo on a failure rather than trusting the
       click. */
    Q_INVOKABLE QString setSound(int slot, const QString &value);
    Q_INVOKABLE void previewSound(int slot);
    Q_INVOKABLE void previewStop();
    /* The operator has moved on: the unanswered-call tone stops. */
    Q_INVOKABLE void silenceFailureTone();
    /* Copies a WAV into the operator's sound directory and selects
       it. Anything else is converted to WAV with ffmpeg first, the
       same as the Qt window does -- the media stack has no other
       decoder -- and rejected if ffmpeg is missing or the conversion
       fails; whatever the media stack then refuses to open is
       rejected too: a sound that cannot be played is worse stored
       than refused. */
    Q_INVOKABLE QString importSound(int slot, const QString &url);
    /* Where the chooser for a sound to import opens, as soundspage.cpp's
       QFileDialog does: the user's music folder. */
    Q_INVOKABLE QUrl musicFolder() const;

    /* Header rules and the ad-hoc headers a call can carry. The rule
       set itself is configuration the core owns; what a frontend can
       do is show which rules fired and attach headers to one call. */
    Q_INVOKABLE QVariantList ruleStats() const;
    /* Header names seen before, so the editor can complete them. */
    Q_INVOKABLE QStringList headerNames() const;
    Q_INVOKABLE bool rememberHeaderName(const QString &name);

    Q_INVOKABLE QVariantList headerPresets() const;
    Q_INVOKABLE bool savePreset(const QString &name,
                                const QStringList &headers);
    Q_INVOKABLE bool deletePreset(const QString &name);
    /* headers are "Name|Value"; they are applied after the configured
       rules, so they always win. */
    Q_INVOKABLE bool dialWithHeaders(const QString &uri,
                                     const QStringList &headers);

    /* The trace for one call, written where the core keeps exports. */
    Q_INVOKABLE QString exportTrace(const QString &callId);

    /* The window remembers its size through the core, so both
       platforms restore the same way -- and on a handset the value is
       simply ignored. */
    Q_INVOKABLE QVariantMap windowSize() const;
    Q_INVOKABLE void saveWindowSize(int w, int h);

    /* A contact's picture as a data: URI, or empty. Encoded here
       because QML takes an image source and the core hands over
       bytes; nothing about the picture is decided on the way. */
    Q_INVOKABLE QString contactPicture(qlonglong id) const;

    Q_INVOKABLE QString buildDate() const;

    /* The About dialog's Copy button: the same plain-text block that
       is on screen, so a copy that read differently from what was
       shown would be worse than no copy button. */
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    /* Keeps the process alive behind an ongoing notification, so the
       registration survives the window going away. Android only; a
       no-op everywhere else, so the window calls it unconditionally. */
    Q_INVOKABLE void setReachable(bool on);

    /* Wifi to LTE, or a cable pulled: the transports are rebuilt and
       every account re-registers. Android calls this from a
       connectivity callback; on the desktop it is a button nobody
       needs often and a call nobody can make too early. */
    Q_INVOKABLE bool networkChanged();

    /* Back, on a handset, hides the window instead of ending the
       application: a softphone that stops being reachable because
       somebody pressed Back is not a phone. A no-op everywhere
       else. */
    Q_INVOKABLE void moveToBackground();

    /* Which side the desktop puts window buttons on. Read from the
       desktop rather than chosen: they were already on one side, and
       moving them because this application started drawing its own
       title bar would be a regression nobody asked for. False on
       Android, where there is no decoration to match. */
    Q_INVOKABLE bool windowButtonsOnLeft() const;

    /* A ringing phone with its window hidden has to say so somewhere
       the operator will see it. Android only; a no-op elsewhere. */
    void announceIncoming(const QString &who);
    void clearIncoming();

/* Deliberately not bridged, and why -- so a later reader does not
 * take the gap for an oversight and add a second way to do something
 * the window already does:
 *
 *   sp_account_rename            reached through sp_account_update,
 *                                which renames in the same step that
 *                                carries the header rules scoped to
 *                                the old name. A direct rename here
 *                                would be a second path with weaker
 *                                guarantees.
 *   sp_media_enum_codecs         sp_media_get_codecs returns the same
 *                                names and everything else the pane
 *                                needs; a second list is a list that
 *                                drifts.
 *   sp_media_set_codec_priority  runtime only, forgotten at the next
 *                                start. A pane driven by it would
 *                                lose the operator's codec order
 *                                every launch; sp_media_set_codecs
 *                                persists and is what the pane uses.
 *   sp_module_is_enabled         sp_modules_get lists every module
 *                                with its state, which is what a list
 *                                of switches needs; asking one at a
 *                                time would be the same answer, once
 *                                per repaint.
 */

signals:
    void themeChanged();
    void startedChanged();
    void accountChanged();
    void lastErrorChanged();
    void callChanged();
    void qualityChanged();
    void logged(const QString &line);
    /* state is an sp_transcript_state; detail says why, for a failure. */
    void transcriptChanged(qlonglong historyId, int state,
                           const QString &detail);

private slots:
    /* Everything the core posts arrives here, on this thread. */
    void handleEvent(int type, qulonglong id, int code, int state,
                     bool flag, const QString &text);

private:
    friend void spBridgeEvent(const sp_event *ev, void *user);
    QColor  hex(const char *v, const char *fallback) const;
    void reloadTheme();
    void refreshRegistration();
    void watchNetwork();
    QString rewritePicture(qlonglong contactId, const QByteArray &png);
    /* Shared by commitPicture and stagedPreview: crop m_staged, scale
       to PICTURE_EDGE and encode as PNG. Empty on a guard failure,
       with the reason in *errorOut. */
    QByteArray encodeStagedCrop(int x, int y, int edge,
                                QString *errorOut) const;
    /* void * so the header need not name a type that only exists in
       the .cpp; every caller is in that file. */
    QString applyCodecs(void *set);
    QString failed(sp_status st) const;

    sp_core *m_core = nullptr;
    sp_theme m_theme {};
    bool     m_haveTheme = false;
    bool     m_started = false;
    bool     m_watchingNetwork = false;
    /* The backend announces the current state when it loads, more
       than once, and none of that is a change. And a handset
       crossing a boundary announces the real one several times,
       which must be a single rebuild. */
    QElapsedTimer m_networkSince;
    QTimer   m_networkSettle;

    sp_account_id m_account = 0;
    bool     m_registered = false;
    QString  m_regDetail;
    QString  m_lastError;
    /* The picture being chosen, decoded once and cropped on commit. */
    QImage   m_staged;

    sp_call_id m_call = 0;
    /* Which call is ringing, so a state change on another one does
       not cancel its notification. */
    sp_call_id m_ringing = 0;
    int      m_callState = 0;
    bool     m_inbound = false;
    QString  m_remoteLabel, m_remoteUri, m_codec, m_quality;
    QString  m_remoteUser, m_contactName, m_transportError, m_cfgPath;
    QStringList m_winLog;
    qreal    m_mos = 0.0;
    bool     m_onHold = false, m_recording = false;
    /* Whether Android's audio is in call mode; see syncAudioMode. */
    bool     m_callAudioMode = false;
#ifdef Q_OS_ANDROID
    /* The proximity wake lock held during a call; see syncAudioMode. */
    QJniObject m_proximity;
#endif
    void     syncAudioMode();
    unsigned m_duration = 0;
    int      m_tx = 0, m_rx = 0;
};

#endif /* COMLINK_COREBRIDGE_H */
