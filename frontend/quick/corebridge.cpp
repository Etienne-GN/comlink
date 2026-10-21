#include "corebridge.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "secretstore.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#endif

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QScreen>
#include <QImageReader>
#include <QList>
#include <QMetaObject>
#include <QProcess>
#include <QElapsedTimer>
#include <QTimer>
#include <QNetworkInformation>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace {
QString str(const char *p) {
    return p ? QString::fromUtf8(p) : QString();
}

/* The key an account's password is filed under: the account's own
   name, which is what survives a restart and what the configuration
   file keys everything else by. */
QString secretKey(const QString &accountId) {
    return QStringLiteral("account/") + accountId;
}

/* stateName in the Qt window, so the display and the log say the
   same word for the same state. */
const char *stateWord(int s) {
    switch (s) {
    case SP_CALL_NULL:         return "idle";
    case SP_CALL_CALLING:      return "calling";
    case SP_CALL_INCOMING:     return "ringing";
    case SP_CALL_EARLY:        return "ringing";
    case SP_CALL_CONNECTING:   return "connecting";
    case SP_CALL_CONFIRMED:    return "connected";
    case SP_CALL_DISCONNECTED: return "ended";
    default:                   return "?";
    }
}
}  // namespace

/* The C callback, on the core's dispatch thread. It copies what it
   needs and hands over; nothing here touches a Qt object directly,
   because this is not the thread that owns them. */
void spBridgeEvent(const sp_event *ev, void *user) {
    auto *self = static_cast<CoreBridge *>(user);
    qulonglong id = 0;
    int code = 0, state = 0;
    bool flag = false;
    QString text;
    char line[SP_QUALITY_LINE_MAX];

    switch (ev->type) {
    case SP_EV_ACCOUNT_STATE:
        id = ev->u.account.acc;
        code = ev->u.account.reg_code;
        flag = ev->u.account.registered;
        text = QString::fromUtf8(ev->u.account.detail ? ev->u.account.detail
                                                      : "");
        break;
    case SP_EV_CALL_STATE:
        id = ev->u.call_state.call;
        state = ev->u.call_state.state;
        code = ev->u.call_state.last_code;
        break;
    case SP_EV_CALL_MEDIA_STATE:
        id = ev->u.media.call;
        flag = ev->u.media.active;
        break;
    case SP_EV_INCOMING_CALL:
        id = ev->u.incoming.call;
        text = QString::fromUtf8(ev->u.incoming.from_uri
                                     ? ev->u.incoming.from_uri : "");
        break;
    case SP_EV_TRANSPORT_ERROR:
        text = QString::fromUtf8(ev->u.transport.detail
                                     ? ev->u.transport.detail : "");
        break;
    case SP_EV_SIP_MESSAGE:
        flag = ev->u.sip.outbound;
        code = ev->u.sip.status_code;
        text = QString::fromUtf8(ev->u.sip.method);
        break;
    case SP_EV_DTMF_RECEIVED:
        id = ev->u.dtmf.call;
        text = QString(QChar(ev->u.dtmf.digit));
        break;
    case SP_EV_XFER_STATUS:
        id = ev->u.xfer.call;
        code = ev->u.xfer.code;
        text = QString::fromUtf8(ev->u.xfer.reason ? ev->u.xfer.reason : "");
        break;
    case SP_EV_LOG:
        code = ev->u.log.level;
        text = QString::fromUtf8(ev->u.log.msg ? ev->u.log.msg : "");
        break;
    case SP_EV_CALL_QUALITY: {
        id = ev->u.quality.call;
        /* The wording is the core's, so the window cannot describe a
           call differently from anything else that reports one. */
        sp_call_quality q {};
        q.mos = ev->u.quality.mos;
        q.rx_loss_pct = ev->u.quality.rx_loss_pct;
        q.rx_jitter.avg_us = ev->u.quality.rx_jitter_us;
        q.rtt.avg_us = ev->u.quality.rtt_us;
        if (sp_call_quality_line(&q, line, sizeof line) == SP_OK)
            text = QString::fromUtf8(line);
        /* Hundredths, because the readout is coloured by it and the
           queued call carries integers. */
        state = (int)(ev->u.quality.mos * 100.0 + 0.5);
        break;
    }
    case SP_EV_TRANSCRIPT:
        id = (qulonglong)ev->u.transcript.history_id;
        state = (int)ev->u.transcript.state;
        text = QString::fromUtf8(ev->u.transcript.detail
                                     ? ev->u.transcript.detail : "");
        break;
    default:
        break;
    }

    /* The notification is posted here, on the core's dispatch thread,
       rather than in handleEvent.
     *
     * Qt's event loop does not run while the activity is stopped, so
     * anything queued to the window's thread waits until somebody
     * opens the window -- which is precisely the case where a
     * notification is the only thing that would have told them to.
     * The core's threads keep running throughout, which is why the
     * phone answers with 100 Trying while the window is away.
     *
     * QJniObject attaches whatever thread calls it. */
    if (ev->type == SP_EV_INCOMING_CALL) {
        self->m_ringing = (sp_call_id)id;
        self->announceIncoming(text);
    } else if (ev->type == SP_EV_CALL_STATE
               && (sp_call_id)id == self->m_ringing
               && ev->u.call_state.state != SP_CALL_INCOMING) {
        /* Only the call that is ringing. Any state change on another
           call used to cancel the notification, so a second call
           arriving while one was up lost its own heads-up. */
        self->m_ringing = 0;
        self->clearIncoming();
    }

    QMetaObject::invokeMethod(self, "handleEvent", Qt::QueuedConnection,
                              Q_ARG(int, (int)ev->type),
                              Q_ARG(qulonglong, id), Q_ARG(int, code),
                              Q_ARG(int, state), Q_ARG(bool, flag),
                              Q_ARG(QString, text));
}

/* The configuration lives where the platform says it should:
   XDG on Debian, the app's own directory on Android. The core takes
   an absolute path and does not care which. */
namespace {
/* Set by main() from --config, before the engine builds the
   singleton. */
QString g_configPath;
}  // namespace

void CoreBridge::setConfigPath(const QString &path) {
    g_configPath = path;
}

CoreBridge::CoreBridge(QObject *parent) : QObject(parent) {
    const QString cfg =
        g_configPath.isEmpty()
            ? QStandardPaths::writableLocation(
                  QStandardPaths::AppConfigLocation)
                  + QStringLiteral("/comlink.yaml")
            : g_configPath;
    const QString dir = QFileInfo(cfg).absolutePath();
    QDir().mkpath(dir);

    if (!QFile::exists(cfg)) {
        QFile f(cfg);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("log_level: 3\nsip_port: 0\n");
            f.write(QStringLiteral("db_file: %1/comlink.db\n")
                        .arg(dir).toUtf8());
            f.write("accounts: []\n");
        }
    }
    m_cfgPath = cfg;
    if (sp_core_create(cfg.toUtf8().constData(), &m_core) == SP_OK) {
        sp_core_set_event_cb(m_core, spBridgeEvent, this);
        connect(this, &CoreBridge::callChanged, this,
                &CoreBridge::syncAudioMode);
        reloadTheme();
    }
}

CoreBridge::~CoreBridge() {
    setReachable(false);
    if (m_core) {
        sp_core_stop(m_core);
        sp_core_destroy(m_core);
    }
}

void CoreBridge::reloadTheme() {
    m_haveTheme = m_core && sp_theme_get(m_core, &m_theme) == SP_OK;
    /* The Qt window's rule: 10pt, Qt's usual default, times the
       core's multiplier, in the theme's family when it names one. */
    QFont f = QGuiApplication::font();
    f.setPointSizeF(basePt());
    if (m_haveTheme && m_theme.font_family && *m_theme.font_family)
        f.setFamily(QString::fromUtf8(m_theme.font_family));
    QGuiApplication::setFont(f);
    emit themeChanged();
}

qreal CoreBridge::basePt() const {
    const double pt = 10.0 * (m_haveTheme ? (double)m_theme.text_multiplier
                                          : 1.0);
#ifdef Q_OS_ANDROID
    /* A point is a length, and Android turns it into pixels at the
       screen's own density, where the desktop turns it into pixels at
       96 per inch. The window is the desktop's layout scaled to the
       screen, so its text has to start from the desktop's pixels:
       the point size is corrected for the difference. */
    if (const QScreen *s = QGuiApplication::primaryScreen())
        if (s->logicalDotsPerInchY() > 0)
            return pt * 96.0 / s->logicalDotsPerInchY();
#endif
    return pt;
}

bool CoreBridge::start() {
    if (!m_core || m_started)
        return m_started;
    if (sp_core_start(m_core) != SP_OK)
        return false;
    m_started = true;
    /* The core adds whatever the configuration file declared, so an
       operator who already has an account keeps it without touching
       the settings pane. */
    sp_account_id ids[32];
    size_t n = 0;
    if (sp_account_list(m_core, ids, 32, &n) == SP_OK && n > 0) {
        m_account = ids[0];
        /* The configuration restored the accounts; the store restores
           what the configuration deliberately does not hold. */
        if (SecretStore::available())
            for (size_t i = 0; i < n && i < 32; i++) {
                const QString name = str(sp_account_id_of(m_core, ids[i]));
                const QString pw = SecretStore::load(secretKey(name));
                if (!pw.isEmpty())
                    sp_account_set_password(m_core, ids[i],
                                            pw.toUtf8().constData());
            }
    }
    reloadTheme();
    setReachable(true);
    watchNetwork();
    note(QStringLiteral("core started from %1").arg(m_cfgPath));
    emit startedChanged();
    emit accountChanged();
    return true;
}

/* Wifi to LTE, a cable pulled, a VPN coming up: every one of them
   leaves the transports bound to an address that has gone, and the
   registration with it. Qt reports the change on both platforms --
   the Android plugin is already in the APK -- so there is no Java
   here and the desktop gets the same behaviour for free. */
void CoreBridge::watchNetwork() {
    if (m_watchingNetwork)
        return;
    if (!QNetworkInformation::loadDefaultBackend())
        return;
    auto *info = QNetworkInformation::instance();
    if (!info)
        return;
    m_watchingNetwork = true;

    /* Coalesced, and never fired for the state the backend reports
       when it loads.
     *
     * Measured on the emulator: Qt announces the current medium and
       the current reachability the moment the backend is ready, both
       of which are the state rather than a change. Rebuilding the
     * transports twice at startup left every one of them marked
     * restarting inside pjsua, and the first call placed was refused
     * with PJSIP_ETPNOTAVAIL -- from a phone that had never left the
     * network it started on.
     *
     * The delay also turns a handset crossing a boundary, which
     * reports the change several times, into one rebuild. */
    m_networkSettle.setSingleShot(true);
    m_networkSettle.setInterval(1500);
    connect(&m_networkSettle, &QTimer::timeout, this, [this]() {
        if (!m_core || !m_started)
            return;
        const bool ok = networkChanged();
        note(ok ? QStringLiteral("network changed: transports rebuilt")
                       : QStringLiteral("network changed: rebuild failed: ")
                             + QString::fromUtf8(
                                   sp_last_error_detail(m_core)));
    });

    /* Every signal the backend emits when it loads describes the
       current state, not a change, and there is more than one of
       them. Counting the first is not enough; nothing within the
       first few seconds of starting is a change. */
    m_networkSince.start();
    const auto changed = [this]() {
        if (m_networkSince.elapsed() < 5000)
            return;
        m_networkSettle.start();
    };
    connect(info, &QNetworkInformation::transportMediumChanged, this,
            changed);
    connect(info, &QNetworkInformation::reachabilityChanged, this,
            [this, changed](QNetworkInformation::Reachability r) {
                /* Only on the way back up: rebuilding transports the
                   moment the network goes away binds them to nothing
                   and the next change has to undo it. */
                if (r == QNetworkInformation::Reachability::Online)
                    changed();
            });
}

/* One account form, marshalled both ways. The buffers have to outlive
   the sp_account_cfg that points into them, which is why this is a
   struct rather than a function returning a cfg. */
namespace {
struct FormBytes {
    QByteArray id, user, domain, authUser, password;
    QByteArray registrar, proxy, stun;

    sp_account_cfg toCfg(const QVariantMap &f) {
        id = f.value("id").toString().trimmed().toUtf8();
        user = f.value("user").toString().trimmed().toUtf8();
        domain = f.value("domain").toString().trimmed().toUtf8();
        authUser = f.value("authUser").toString().trimmed().toUtf8();
        password = f.value("password").toString().toUtf8();
        registrar = f.value("registrar").toString().trimmed().toUtf8();
        proxy = f.value("proxy").toString().trimmed().toUtf8();
        stun = f.value("stun").toString().trimmed().toUtf8();

        sp_account_cfg c {};
        c.id = id.constData();
        c.user = user.constData();
        c.domain = domain.constData();
        /* Empty means "same as user", which is what the core's NULL
           means; sending "" instead would authenticate as nobody. */
        c.auth_user = authUser.isEmpty() ? user.constData()
                                         : authUser.constData();
        c.password = password.isEmpty() ? nullptr : password.constData();
        c.registrar = registrar.isEmpty() ? nullptr : registrar.constData();
        c.outbound_proxy = proxy.isEmpty() ? nullptr : proxy.constData();
        c.stun_server = stun.isEmpty() ? nullptr : stun.constData();
        c.transport = (sp_transport)f.value("transport", 0).toInt();
        c.srtp = (sp_srtp_mode)f.value("srtp", 0).toInt();
        c.reg_expires_s = f.value("expires", 0).toInt();
        c.do_register = f.value("doRegister", true).toBool();
        return c;
    }
};

}  // namespace

QVariantList CoreBridge::accounts() const {
    QVariantList rows;
    if (!m_core)
        return rows;
    sp_account_id ids[32];
    size_t n = 0;
    if (sp_account_list(m_core, ids, 32, &n) != SP_OK)
        return rows;
    for (size_t i = 0; i < n && i < 32; i++) {
        sp_account_state st {};
        sp_account_get_state(m_core, ids[i], &st);
        QVariantMap row;
        row["id"] = (qlonglong)ids[i];
        row["name"] = str(sp_account_id_of(m_core, ids[i]));
        row["domain"] = str(sp_account_domain_of(m_core, ids[i]));
        row["registered"] = st.registered;
        row["detail"] = str(st.detail);
        row["regCode"] = st.reg_code;
        row["current"] = ids[i] == m_account;
        rows << row;
    }
    return rows;
}

qlonglong CoreBridge::currentAccount() const {
    return (qlonglong)m_account;
}

void CoreBridge::setCurrentAccount(qlonglong id) {
    if ((sp_account_id)id == m_account)
        return;
    m_account = (sp_account_id)id;
    refreshRegistration();
    emit accountChanged();
}

bool CoreBridge::credentialStoreAvailable() const {
    return SecretStore::available();
}

QString CoreBridge::credentialStore() const {
    return SecretStore::describe();
}

QVariantMap CoreBridge::accountCfg(qlonglong id) const {
    QVariantMap f;
    /* The blank form is the core's defaults, not this file's: an
       account added with these values is the one the core would have
       made from a bare struct. */
    f["id"] = QString();
    f["user"] = QString();
    f["domain"] = QString();
    f["authUser"] = QString();
    f["password"] = QString();
    f["registrar"] = QString();
    f["proxy"] = QString();
    f["stun"] = QString();
    f["transport"] = 0;
    f["srtp"] = 0;
    f["expires"] = 0;
    f["doRegister"] = true;
    f["persist"] = true;
    f["passwordKnown"] = true;
    if (!m_core || id == 0)
        return f;

    sp_account_cfg c {};
    if (sp_account_get_cfg(m_core, (sp_account_id)id, &c) != SP_OK)
        return f;
    f["id"] = str(c.id);
    f["user"] = str(c.user);
    f["domain"] = str(c.domain);
    f["authUser"] = str(c.auth_user);
    /* From the store when there is one: the core has no password to
       give back, because it was never given one.
     *
     * passwordKnown says whether that answer is a fact. A store that
     * could not be read hands back an empty string exactly like a
     * store with nothing in it, and a form that cannot tell them
     * apart is a form that erases the password it failed to read. */
    if (SecretStore::available()) {
        bool known = false;
        const QString kept = SecretStore::load(secretKey(str(c.id)), &known);
        if (!kept.isEmpty()) {
            f["password"] = kept;
            f["passwordKnown"] = true;
        } else if (known && c.password && *c.password) {
            /* Nothing in the store, and the configuration still
               carries one: an installation that predates the keyring,
               which is every installation there is. Showing it is
               what makes the first save move it across instead of
               erasing it -- which is what happened, on the operator's
               first look at the new window. */
            f["password"] = str(c.password);
            f["passwordKnown"] = true;
        } else {
            f["password"] = QString();
            f["passwordKnown"] = known;
        }
    } else {
        f["password"] = str(c.password);
        f["passwordKnown"] = true;
    }
    f["registrar"] = str(c.registrar);
    f["proxy"] = str(c.outbound_proxy);
    f["stun"] = str(c.stun_server);
    f["transport"] = (int)c.transport;
    f["srtp"] = (int)c.srtp;
    f["expires"] = c.reg_expires_s;
    f["doRegister"] = c.do_register;
    return f;
}

QString CoreBridge::validateAccount(const QVariantMap &form) const {
    FormBytes bytes;
    sp_account_cfg cfg = bytes.toCfg(form);
    char why[256] = {0};
    sp_account_validate(&cfg, why, sizeof why);
    return QString::fromUtf8(why);
}

qlonglong CoreBridge::saveAccount(qlonglong id, const QVariantMap &form) {
    if (!m_core)
        return 0;
    FormBytes bytes;
    sp_account_cfg cfg = bytes.toCfg(form);
    char why[256] = {0};
    if (sp_account_validate(&cfg, why, sizeof why) != SP_OK) {
        m_lastError = QString::fromUtf8(why);
        emit lastErrorChanged();
        return 0;
    }

    /* The account goes to the core; the password goes to the store.
       The core writes everything it is given into comlink.yaml, so an
       account handed a password is an account whose password is on
       disk in cleartext -- which is why it is handed none, and given
       one afterwards through sp_account_set_password. */
    const QString password = form.value("password").toString();
    const bool keepSecret = SecretStore::available();
    if (keepSecret)
        cfg.password = nullptr;
    /* Read before anything is changed: after an update there is no
       old account left to ask what it was called. */
    const QString oldName =
        id == 0 ? QString() : str(sp_account_id_of(m_core, (sp_account_id)id));

    sp_account_id out = 0;
    sp_status st;
    if (id == 0) {
        /* persist=false stores nothing at all, not even the account:
           it exists for this run and is gone at the next launch. */
        st = form.value("persist", true).toBool()
                 ? sp_account_add(m_core, &cfg, &out)
                 : sp_account_add_transient(m_core, &cfg, &out);
    } else {
        size_t rules = 0;
        st = sp_account_update(m_core, (sp_account_id)id, &cfg, &out, &rules);
    }
    if (st != SP_OK && out == 0) {
        m_lastError = QStringLiteral("%1 %2")
                          .arg(QString::fromUtf8(sp_strerror(st)),
                               QString::fromUtf8(sp_last_error_detail(m_core)))
                          .trimmed();
        return 0;
    }

    /* A rename that failed leaves the account added and only the name
       unchanged, and sp_account_update says so by handing back a live
       handle with a non-OK status. Treating that as total failure
       left the window pointing at the handle the update retired, and
       every call afterwards refused for no visible reason. */
    const QString renameProblem =
        st == SP_OK ? QString()
                    : QStringLiteral("%1 %2")
                          .arg(QString::fromUtf8(sp_strerror(st)),
                               QString::fromUtf8(sp_last_error_detail(m_core)))
                          .trimmed();

    QString secretProblem;
    if (keepSecret) {
        const QString name = form.value("id").toString().trimmed();
        const QString key = secretKey(name);
        /* An empty field is only an instruction to forget the
           password when the form knew what the password was. If the
           store could not be read, the field is empty because of
           that, and erasing here would destroy what it failed to
           show. */
        const bool known = form.value("passwordKnown", true).toBool();
        bool kept = true;
        if (!password.isEmpty())
            kept = SecretStore::store(key, password);
        else if (known)
            SecretStore::erase(key);

        if (!kept)
            /* Said out loud rather than swallowed: the account works
               for this run and would come back without a password at
               the next launch, which is the one failure that looks
               like success until tomorrow. */
            secretProblem = tr("The account was saved, but its password "
                               "could not be kept in %1.")
                                .arg(SecretStore::describe());
        /* The old entry, when a rename moved the account: a SIP
           credential left behind under a name nothing refers to is a
           credential nobody will ever clear. */
        if (oldName != name && !oldName.isEmpty())
            SecretStore::erase(secretKey(oldName));

        if (!password.isEmpty() || known)
            sp_account_set_password(m_core, out,
                                    password.isEmpty()
                                        ? nullptr
                                        : password.toUtf8().constData());
    }
    /* The account is live either way, so the window follows it. What
       a partial failure changes is what the window says, not whether
       it knows which account it is on. */
    if (m_account == 0 || (sp_account_id)id == m_account)
        m_account = out;
    m_lastError = !renameProblem.isEmpty() ? renameProblem : secretProblem;
    emit lastErrorChanged();
    refreshRegistration();
    emit accountChanged();
    return (qlonglong)out;
}

bool CoreBridge::removeAccount(qlonglong id) {
    if (!m_core || id == 0)
        return false;
    /* Read before the removal, because afterwards there is no account
       to ask for its name. A password left behind for an account
       nobody has is a password nobody will ever clear. */
    const QString name = str(sp_account_id_of(m_core, (sp_account_id)id));
    if (sp_account_remove(m_core, (sp_account_id)id) != SP_OK) {
        m_lastError = QString::fromUtf8(sp_last_error_detail(m_core));
        emit lastErrorChanged();
        return false;
    }
    if (SecretStore::available() && !name.isEmpty())
        SecretStore::erase(secretKey(name));
    if ((sp_account_id)id == m_account) {
        /* Something has to be dialable afterwards, so the selection
           falls to whatever is left rather than to nothing. */
        m_account = 0;
        sp_account_id ids[32];
        size_t n = 0;
        if (sp_account_list(m_core, ids, 32, &n) == SP_OK && n > 0)
            m_account = ids[0];
    }
    refreshRegistration();
    emit accountChanged();
    return true;
}

bool CoreBridge::setRegistration(qlonglong id, bool on) {
    if (!m_core || id == 0)
        return false;
    const QString name = str(sp_account_id_of(m_core, (sp_account_id)id));
    const sp_status st =
        sp_account_set_registration(m_core, (sp_account_id)id, on);
    if (st != SP_OK) {
        m_lastError = QString::fromUtf8(sp_last_error_detail(m_core));
        /* A REGISTER still in flight is told apart from a transport
           failure: retry, do not go inspect the network. */
        note(st == SP_ERR_STATE
                 ? QStringLiteral("%1: %2").arg(name, m_lastError)
                 : QStringLiteral("%1: registration change failed: %2 (%3)")
                       .arg(name, QString::fromUtf8(sp_strerror(st)),
                            m_lastError));
        emit lastErrorChanged();
        return false;
    }
    note(QStringLiteral("%1 account \"%2\"")
             .arg(on ? QStringLiteral("registering")
                     : QStringLiteral("unregistering"), name));
    m_lastError.clear();
    emit lastErrorChanged();
    refreshRegistration();
    emit accountChanged();
    return true;
}

/* The status row shows the selected account, so switching accounts has
   to bring its registration with it rather than leave the last one's
   on screen. */
void CoreBridge::refreshRegistration() {
    sp_account_state st {};
    if (m_core && m_account &&
        sp_account_get_state(m_core, m_account, &st) == SP_OK) {
        m_registered = st.registered;
        m_regDetail = str(st.detail);
    } else {
        m_registered = false;
        m_regDetail.clear();
    }
}

bool CoreBridge::dial(const QString &uri) {
    if (!m_core || m_account == 0) {
        note(QStringLiteral("no account selected"));
        m_lastError = tr("No account to call from.");
        emit lastErrorChanged();
        emit callChanged();
        return false;
    }
    /* "123" becomes sip:123@<the account's domain>, which is the
       core's rule rather than this file's: what three keystrokes mean
       is a product decision, and two frontends deciding it
       differently is how one platform dials a different extension
       from the other. */
    char full[512];
    const sp_status cs = sp_call_uri_complete(m_core, m_account,
                                              uri.toUtf8().constData(),
                                              full, sizeof full);
    if (cs != SP_OK) {
        m_lastError = failed(cs);
        emit lastErrorChanged();
        emit callChanged();
        return false;
    }

    sp_call_id call = 0;
    /* A refused call has to say so. It failed silently before, which
       on a laptop looks exactly like a keyboard that did nothing --
       and the first thing a wrong domain does is refuse. */
    const sp_status st = sp_call_make(m_core, m_account, full, nullptr, 0,
                                      &call);
    if (st != SP_OK) {
        m_lastError = failed(st);
        emit lastErrorChanged();
        note(QStringLiteral("call failed: %1 (%2)")
                 .arg(QString::fromUtf8(sp_strerror(st)),
                      QString::fromUtf8(sp_last_error_detail(m_core))));
        emit callChanged();
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    note(QStringLiteral("calling %1").arg(QString::fromUtf8(full)));
    m_call = call;
    refreshCall();
    return true;
}

void CoreBridge::answer() {
    if (m_core && m_call)
        sp_call_answer(m_core, m_call, 200, nullptr, 0);
}

void CoreBridge::hangup(int code) {
    if (m_core && m_call)
        sp_call_hangup(m_core, m_call, code);
}

void CoreBridge::dtmf(const QString &digits) {
    if (m_core && m_call)
        sp_call_send_dtmf(m_core, m_call, digits.toUtf8().constData());
}

QString CoreBridge::enterDigit(const QString &digit) {
    if (digit.isEmpty())
        return QString();
    /* Confirmed, not merely existing: a digit typed while a call is
       still ringing belongs in the number, because there is no media
       to carry a tone yet. */
    if (m_core && m_call && m_callState == SP_CALL_CONFIRMED) {
        sp_call_send_dtmf(m_core, m_call, digit.toUtf8().constData());
        return QString();
    }
    return digit;
}

void CoreBridge::setHold(bool hold) {
    if (m_core && m_call)
        sp_call_set_hold(m_core, m_call, hold);
    refreshCall();
}

bool CoreBridge::transfer(const QString &uri) {
    if (!m_core || !m_call || uri.isEmpty())
        return false;
    return sp_call_xfer_blind(m_core, m_call,
                              uri.toUtf8().constData()) == SP_OK;
}

QString CoreBridge::toggleRecording() {
    if (!m_core || !m_call)
        return QString();
    if (m_recording) {
        sp_call_record_stop(m_core, m_call);
        refreshCall();
        return QStringLiteral("recording stopped");
    }
    const char *path = nullptr;
    /* _auto, so the name and the directory are the core's -- the
       recording settings page exists to change them in one place. */
    if (sp_call_record_start_auto(m_core, m_call, &path) != SP_OK)
        return QStringLiteral("could not start recording");
    const QString p = QString::fromUtf8(path ? path : "");
    refreshCall();
    return p;
}

void CoreBridge::pollSignal() {
    sp_signal_level lv {};
    if (!m_core || !m_call
        || sp_call_get_signal(m_core, m_call, &lv) != SP_OK || !lv.valid) {
        m_tx = m_rx = -1;
        return;
    }
    m_tx = (int)lv.tx;
    m_rx = (int)lv.rx;
}

/* The call display asks the core what to show rather than assembling
   it from events: remote_label especially, since the core has already
   decided what a caller is called. */
QVariantList CoreBridge::calls() const {
    QVariantList out;
    if (!m_core)
        return out;
    sp_call_id ids[16];
    size_t n = 0;
    if (sp_call_list(m_core, ids, 16, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n && i < 16; i++) {
        sp_call_info info {};
        if (sp_call_get_info(m_core, ids[i], &info) != SP_OK)
            continue;
        QVariantMap m;
        m["id"] = (qlonglong)ids[i];
        m["state"] = (int)info.state;
        m["label"] = QString::fromUtf8(info.remote_label ? info.remote_label
                                                         : "");
        m["uri"] = QString::fromUtf8(info.remote_uri ? info.remote_uri : "");
        m["user"] = QString::fromUtf8(info.remote_user ? info.remote_user : "");
        const sp_contact *known = nullptr;
        m["contact"] = info.remote_uri
                           && sp_contact_lookup(m_core, info.remote_uri, &known)
                                  == SP_OK
                           && known && known->name
                           ? QString::fromUtf8(known->name) : QString();
        m["stateName"] = QString::fromUtf8(stateWord(info.state));
        m["onHold"] = info.on_hold;
        m["recording"] = info.recording;
        m["inbound"] = info.inbound;
        m["duration"] = (int)info.duration_s;
        m["codec"] = QString::fromUtf8(info.codec ? info.codec : "");
        m["current"] = ids[i] == m_call;
        out.append(m);
    }
    return out;
}

qlonglong CoreBridge::currentCall() const {
    return (qlonglong)m_call;
}

void CoreBridge::setCurrentCall(qlonglong id) {
    m_call = (sp_call_id)id;
    refreshCall();
}

bool CoreBridge::xferAttended(qlonglong a, qlonglong b) {
    return m_core && a != 0 && b != 0
        && sp_call_xfer_attended(m_core, (sp_call_id)a, (sp_call_id)b)
               == SP_OK;
}

void CoreBridge::refreshCall() {
    if (!m_core || !m_call) {
        m_callState = 0;
        m_remoteLabel.clear();
        m_remoteUri.clear();
        m_remoteUser.clear();
        m_contactName.clear();
        m_codec.clear();
        m_quality.clear();
        m_mos = 0.0;
        emit qualityChanged();
        m_duration = 0;
        m_onHold = false;
        m_recording = false;
        emit callChanged();
        return;
    }
    sp_call_info i {};
    if (sp_call_get_info(m_core, m_call, &i) != SP_OK)
        return;
    m_callState = (int)i.state;
    m_inbound = i.inbound;
    m_onHold = i.on_hold;
    m_recording = i.recording;
    m_remoteUri = QString::fromUtf8(i.remote_uri ? i.remote_uri : "");
    /* A number in the book shows the name it was saved under. The
       lookup is live and belongs here rather than in
       sp_call_info.remote_label, which the call history also stores:
       a past record must not change its mind about who was called
       because the book was edited afterwards. */
    m_remoteLabel = QString::fromUtf8(i.remote_label ? i.remote_label : "");
    m_remoteUser = QString::fromUtf8(i.remote_user ? i.remote_user : "");
    m_contactName.clear();
    const sp_contact *known = nullptr;
    if (!m_remoteUri.isEmpty()
        && sp_contact_lookup(m_core, m_remoteUri.toUtf8().constData(), &known)
               == SP_OK
        && known && known->name && *known->name) {
        m_remoteLabel = QString::fromUtf8(known->name);
        m_contactName = m_remoteLabel;
    }
    m_codec = QString::fromUtf8(i.codec ? i.codec : "");
    m_duration = i.duration_s;
    emit callChanged();
}

void CoreBridge::handleEvent(int type, qulonglong id, int code, int state,
                             bool flag, const QString &text) {
    switch (type) {
    case SP_EV_ACCOUNT_STATE:
        if ((sp_account_id)id == m_account) {
            m_registered = flag;
            m_regDetail = flag ? QString()
                               : QStringLiteral("%1%2").arg(code)
                                     .arg(text.isEmpty() ? QString()
                                                         : ": " + text);
        }
        m_transportError.clear();
        note(QStringLiteral("account %1: %2 (%3) %4")
                 .arg(id)
                 .arg(flag ? QStringLiteral("registered")
                           : QStringLiteral("not registered"))
                 .arg(code)
                 .arg(text));
        emit accountChanged();
        break;
    case SP_EV_INCOMING_CALL:
        /* Shown only when nothing is: a second call arriving while
           one is answered joins the list rather than replacing what
           is on screen mid-conversation. */
        if (m_call == 0)
            m_call = (sp_call_id)id;
        note(QStringLiteral("incoming call #%1 from %2").arg(id).arg(text));
        refreshCall();
        break;
    case SP_EV_CALL_STATE:
        note(QStringLiteral("call #%1 %2 (%3)")
                 .arg(id)
                 .arg(QString::fromUtf8(stateWord(state)))
                 .arg(code));
        if ((sp_call_id)id == m_call) {
            if (state == SP_CALL_DISCONNECTED) {
                /* The display falls to whatever is still live rather
                   than to nothing: hanging up the consultation leg
                   should put the held party back on screen. */
                m_call = 0;
                sp_call_id ids[16];
                size_t n = 0;
                if (sp_call_list(m_core, ids, 16, &n) == SP_OK)
                    for (size_t i = 0; i < n && i < 16; i++)
                        if (ids[i] != (sp_call_id)id) {
                            m_call = ids[i];
                            break;
                        }
            }
            refreshCall();
        } else {
            /* Another call changed: the list has to redraw even when
               the display did not. */
            emit callChanged();
        }
        break;
    case SP_EV_CALL_MEDIA_STATE:
        note(QStringLiteral("call #%1 audio %2")
                 .arg(id)
                 .arg(flag ? QStringLiteral("active")
                           : QStringLiteral("inactive")));
        break;
    case SP_EV_DTMF_RECEIVED:
        note(QStringLiteral("call #%1 received DTMF %2").arg(id).arg(text));
        break;
    case SP_EV_XFER_STATUS:
        note(QStringLiteral("transfer of #%1: %2 %3")
                 .arg(id).arg(code).arg(text));
        break;
    case SP_EV_CALL_QUALITY:
        /* The selected call when there is one, otherwise whatever is
           reporting. */
        if (m_call == 0 || (sp_call_id)id == m_call) {
            m_quality = text;
            m_mos = state / 100.0;
            emit qualityChanged();
        }
        break;
    case SP_EV_TRANSPORT_ERROR:
        /* A failed TLS transport does not fall back to UDP, so the
           dot has to say why nothing registered. */
        m_transportError = text;
        note(QStringLiteral("TRANSPORT ERROR: %1").arg(text));
        emit accountChanged();
        break;
    case SP_EV_SIP_MESSAGE:
        /* Not written to the activity pane: one line per SIP message
           would drown everything else. The SIP trace shows these. */
        break;
    case SP_EV_LOG:
        note(text);
        break;
    case SP_EV_TRANSCRIPT:
        emit transcriptChanged((qlonglong)id, state, text);
        break;
    default:
        break;
    }
}

void CoreBridge::note(const QString &text) {
    const QString line =
        QStringLiteral("%1  %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                 text);
    m_winLog.append(line);
    /* The Qt window's pane keeps 500 blocks. */
    while (m_winLog.size() > 500)
        m_winLog.removeFirst();
    emit logged(line);
}

QStringList CoreBridge::windowLog() const {
    return m_winLog;
}

QString CoreBridge::accountLabel() const {
    if (!m_core || m_account == 0)
        return m_started ? QStringLiteral("no account")
                         : QStringLiteral("not started");
    const char *id = sp_account_id_of(m_core, m_account);
    const char *dom = sp_account_domain_of(m_core, m_account);
    return QStringLiteral("%1@%2").arg(QString::fromUtf8(id ? id : "?"),
                                       QString::fromUtf8(dom ? dom : "?"));
}

QString CoreBridge::callTitle() const {
    if (ringing())
        return QStringLiteral("Incoming call");
    if (!m_call || m_callState == SP_CALL_DISCONNECTED)
        return QStringLiteral("No call");
    return QString::fromUtf8(stateWord(m_callState));
}

bool CoreBridge::ringing() const {
    return m_call && m_inbound
        && (m_callState == SP_CALL_INCOMING || m_callState == SP_CALL_EARLY);
}

QString CoreBridge::clock() const {
    if (m_callState != SP_CALL_CONFIRMED)
        return QString();
    return QStringLiteral("%1:%2")
        .arg(m_duration / 60, 2, 10, QLatin1Char('0'))
        .arg(m_duration % 60, 2, 10, QLatin1Char('0'));
}

int CoreBridge::startPane() const {
    return qEnvironmentVariableIntValue("COMLINK_QUICK_PANE");
}

QString CoreBridge::startOverlay() const {
    return qEnvironmentVariable("COMLINK_QUICK_OVERLAY");
}

QString CoreBridge::startDial() const {
    return qEnvironmentVariable("COMLINK_QUICK_DIAL");
}

QString CoreBridge::version() const {
    return QString::fromUtf8(sp_build()->version);
}
QString CoreBridge::pjproject() const {
    return QString::fromUtf8(sp_build()->pjproject_version);
}
QString CoreBridge::commit() const {
    return QString::fromUtf8(sp_build()->git_commit);
}
QString CoreBridge::qtVersion() const {
    return QString::fromUtf8(qVersion());
}
/* The default the core uses when db_file is not set in the
   configuration: beside it, named comlink.db. Said as "default"
   rather than asserted, because a config can point it elsewhere and
   the core does not expose the resolved path -- aboutdialog.cpp's own
   words for the same qualifier. */
QString CoreBridge::databasePath() const {
    return QFileInfo(m_cfgPath).absolutePath() + QStringLiteral("/comlink.db");
}
QString CoreBridge::soundDir() const {
    QString dir = qEnvironmentVariable("COMLINK_SOUND_DIR");
    if (dir.isEmpty())
        dir = QStringLiteral("/usr/share/comlink/sounds");
    return dir;
}

/* The fallbacks are the Qt frontend's own, for the moment before a
   core exists: a window painted white for that moment is a flash of
   daylight on fifteen of the sixteen presets. */
QColor CoreBridge::hex(const char *v, const char *fallback) const {
    return QColor(QString::fromUtf8(v && *v ? v : fallback));
}

QColor CoreBridge::accent() const {
    return hex(m_haveTheme ? m_theme.accent_hex : nullptr, "#378ADD");
}
QColor CoreBridge::background() const {
    return hex(m_haveTheme ? m_theme.background_hex : nullptr, "#141820");
}
QColor CoreBridge::surface() const {
    return hex(m_haveTheme ? m_theme.surface_hex : nullptr, "#1C2027");
}
QColor CoreBridge::foreground() const {
    return hex(m_haveTheme ? m_theme.foreground_hex : nullptr, "#E6E9EF");
}
QColor CoreBridge::muted() const {
    return hex(m_haveTheme ? m_theme.muted_hex : nullptr, "#8A93A3");
}
bool CoreBridge::dark() const {
    return m_haveTheme ? m_theme.dark : true;
}

QColor CoreBridge::go() const {
    char h[8];
    if (!m_haveTheme
        || sp_theme_semantic(&m_theme, SP_SEMANTIC_GO, h, sizeof h) != SP_OK)
        return QColor(QStringLiteral("#52DA84"));
    return QColor(QString::fromUtf8(h));
}
QColor CoreBridge::stop() const {
    char h[8];
    if (!m_haveTheme
        || sp_theme_semantic(&m_theme, SP_SEMANTIC_STOP, h, sizeof h) != SP_OK)
        return QColor(QStringLiteral("#DA5B52"));
    return QColor(QString::fromUtf8(h));
}
QColor CoreBridge::txTrace() const {
    char h[8];
    if (!m_haveTheme || sp_theme_channel(&m_theme, false, h, sizeof h) != SP_OK)
        return accent();
    return QColor(QString::fromUtf8(h));
}
QColor CoreBridge::rxTrace() const {
    char h[8];
    if (!m_haveTheme || sp_theme_channel(&m_theme, true, h, sizeof h) != SP_OK)
        return QColor(QStringLiteral("#DD37DD"));
    return QColor(QString::fromUtf8(h));
}

QString CoreBridge::initialsOf(const QString &name) const {
    char out[SP_INITIALS_MAX];
    if (sp_contact_initials(name.toUtf8().constData(), out, sizeof out)
            != SP_OK)
        return QString();
    return QString::fromUtf8(out);
}

/* --- the panes -------------------------------------------------------
 *
 * Rows, not widgets. Each of these hands QML what the core already
 * decided -- the label a contact is known by, the order history comes
 * back in, the words an action log entry uses -- so the window
 * renders and does not choose. */

QVariantList CoreBridge::contacts(const QString &search) const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_contact *rows = nullptr;
    size_t n = 0;
    const QByteArray s = search.toUtf8();
    if (sp_contacts_read(m_core, s.isEmpty() ? nullptr : s.constData(),
                         200, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["id"] = (qlonglong)rows[i].id;
        m["name"] = QString::fromUtf8(rows[i].name ? rows[i].name : "");
        m["company"] = QString::fromUtf8(rows[i].company ? rows[i].company
                                                          : "");
        m["favourite"] = rows[i].favourite;
        /* The first number is what a list row dials; the editor is
           where the rest live. */
        m["uri"] = rows[i].n_numbers > 0 && rows[i].numbers[0].uri
                       ? QString::fromUtf8(rows[i].numbers[0].uri)
                       : QString();
        m["numbers"] = (int)rows[i].n_numbers;
        out.append(m);
    }
    return out;
}

QVariantList CoreBridge::history(int limit) const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_call_record *rows = nullptr;
    size_t n = 0;
    if (sp_history_read(m_core, (size_t)limit, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["id"] = (qlonglong)rows[i].id;
        m["inbound"] = rows[i].inbound;
        m["remote"] = QString::fromUtf8(rows[i].remote_uri
                                            ? rows[i].remote_uri : "");
        m["startUs"] = (qulonglong)rows[i].start_us;
        m["duration"] = (int)rows[i].duration_s;
        m["code"] = rows[i].disconnect_code;
        m["codec"] = QString::fromUtf8(rows[i].codec ? rows[i].codec : "");
        /* Answered at all, which is what distinguishes a missed call
           from a short one. */
        m["answered"] = rows[i].answer_us != 0;
        m["recording"] = QString::fromUtf8(rows[i].recording_path
                                               ? rows[i].recording_path : "");
        m["callId"] = QString::fromUtf8(rows[i].sip_call_id
                                            ? rows[i].sip_call_id : "");
        m["mos"] = rows[i].q_mos;
        m["qPackets"] = (int)rows[i].q_rx_packets;
        /* Only meaningful alongside qPackets > 0, the same guard
           logsdialog.cpp's own "MOS %1  loss %2%" line uses. */
        m["qLossPct"] = rows[i].q_rx_loss_pct;
        m["transcript"] = (int)rows[i].transcript;
        m["transcriptWhy"] = QString::fromUtf8(rows[i].transcript_detail
                                               ? rows[i].transcript_detail
                                               : "");
        out.append(m);
    }
    return out;
}

QVariantList CoreBridge::activity(int limit) const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_action_entry *rows = nullptr;
    size_t n = 0;
    if (sp_actionlog_read(m_core, (size_t)limit, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["tsUs"] = (qulonglong)rows[i].ts_us;
        m["level"] = rows[i].level;
        m["text"] = QString::fromUtf8(rows[i].text ? rows[i].text : "");
        out.append(m);
    }
    return out;
}

/* The full figures, for the pane that exists to show them. The call
   display's one-line summary is sp_call_quality_line. */
QVariantMap CoreBridge::quality() const {
    QVariantMap m;
    sp_call_quality q {};
    if (!m_core || !m_call || sp_call_get_quality(m_core, m_call, &q) != SP_OK
        || !q.valid) {
        m["valid"] = false;
        return m;
    }
    m["valid"] = true;
    m["mos"] = q.mos;
    m["rFactor"] = q.r_factor;
    m["rxLoss"] = q.rx_loss_pct;
    m["txLoss"] = q.tx_loss_pct;
    m["rxPackets"] = (qulonglong)q.rx_packets;
    m["txPackets"] = (qulonglong)q.tx_packets;
    m["rxJitterAvgMs"] = q.rx_jitter.avg_us / 1000.0;
    m["rxJitterMaxMs"] = q.rx_jitter.max_us / 1000.0;
    m["txJitterAvgMs"] = q.tx_jitter.avg_us / 1000.0;
    m["rxLost"] = (qulonglong)q.rx_lost;
    m["txLost"] = (qulonglong)q.tx_lost;
    m["rttAvgMs"] = q.rtt.avg_us / 1000.0;
    m["rttMaxMs"] = q.rtt.max_us / 1000.0;
    m["jbAvgDelayMs"] = (int)q.jb_avg_delay_ms;
    m["jbLost"] = (qulonglong)q.jb_lost;
    m["jbDiscarded"] = (qulonglong)q.jb_discarded;
    m["final"] = q.final;
    return m;
}

/* The Quality dialog's fourteen-row table, for one call picked from
   its own combo -- not necessarily the call this bridge otherwise
   treats as "current". Every figure is sp_call_quality's, unrounded
   past the millisecond conversion QML would otherwise have to redo
   per row. */
QVariantMap CoreBridge::qualityDetail(qlonglong callId) const {
    QVariantMap m;
    m["exists"] = false;
    m["valid"] = false;
    if (!m_core || callId == 0)
        return m;
    sp_call_quality q {};
    if (sp_call_get_quality(m_core, (sp_call_id)callId, &q) != SP_OK)
        return m;
    m["exists"] = true;
    m["valid"] = q.valid;
    if (!q.valid)
        return m;
    m["final"] = q.final;
    m["mos"] = q.mos;
    m["rFactor"] = q.r_factor;
    m["rxPackets"] = (qulonglong)q.rx_packets;
    m["txPackets"] = (qulonglong)q.tx_packets;
    m["rxBytes"] = (qulonglong)q.rx_bytes;
    m["txBytes"] = (qulonglong)q.tx_bytes;
    m["rxLost"] = (qulonglong)q.rx_lost;
    m["rxLossPct"] = q.rx_loss_pct;
    m["txLost"] = (qulonglong)q.tx_lost;
    m["txLossPct"] = q.tx_loss_pct;
    m["rxDiscarded"] = (qulonglong)q.rx_discarded;
    m["rxReordered"] = (qulonglong)q.rx_reordered;
    m["rxDuplicated"] = (qulonglong)q.rx_duplicated;
    const auto ms = [](const sp_metric_us &v) {
        QVariantMap r;
        r["cur"] = v.cur_us / 1000.0;
        r["min"] = v.min_us / 1000.0;
        r["max"] = v.max_us / 1000.0;
        r["avg"] = v.avg_us / 1000.0;
        return r;
    };
    m["rxJitter"] = ms(q.rx_jitter);
    m["txJitter"] = ms(q.tx_jitter);
    m["rtt"] = ms(q.rtt);
    m["jbSizeFrames"] = (qulonglong)q.jb_size_frames;
    m["jbPrefetchFrames"] = (qulonglong)q.jb_prefetch_frames;
    m["jbAvgDelayMs"] = (qulonglong)q.jb_avg_delay_ms;
    m["jbMinDelayMs"] = (qulonglong)q.jb_min_delay_ms;
    m["jbMaxDelayMs"] = (qulonglong)q.jb_max_delay_ms;
    m["jbLost"] = (qulonglong)q.jb_lost;
    m["jbDiscarded"] = (qulonglong)q.jb_discarded;
    m["jbEmpty"] = (qulonglong)q.jb_empty;
    return m;
}

/* --- settings --------------------------------------------------------
 *
 * Every list here is the core's, including the spelling of a preset
 * and which codec parameters a codec actually supports. A frontend
 * that kept its own would be a frontend that drifts the moment one is
 * added. */

QVariantList CoreBridge::themePresets() const {
    QVariantList out;
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        QVariantMap m;
        m["index"] = i;
        m["label"] = QString::fromUtf8(
            sp_theme_preset_label((sp_theme_preset)i));
        out.append(m);
    }
    return out;
}

int CoreBridge::currentPreset() const {
    return m_haveTheme ? (int)m_theme.preset : 0;
}

void CoreBridge::setPreset(int preset) {
    if (!m_core)
        return;
    if (sp_theme_set(m_core, (sp_theme_preset)preset, nullptr, nullptr,
                     m_haveTheme ? m_theme.text_scale : SP_TEXT_MEDIUM)
        == SP_OK)
        const_cast<CoreBridge *>(this)->reloadTheme();
}

/* One theme, as a map. Every colour the window uses comes from here
   or from the properties that read the same struct, so a preview and
   the live theme cannot disagree about what a preset looks like. */
static QVariantMap themeMap(const sp_theme &t) {
    QVariantMap m;
    const auto s = [](const char *p) {
        return p ? QString::fromUtf8(p) : QString();
    };
    m["preset"] = (int)t.preset;
    m["scale"] = (int)t.text_scale;
    m["multiplier"] = (qreal)t.text_multiplier;
    m["dark"] = t.dark;
    m["accent"] = s(t.accent_hex);
    m["background"] = s(t.background_hex);
    m["surface"] = s(t.surface_hex);
    m["foreground"] = s(t.foreground_hex);
    m["muted"] = s(t.muted_hex);
    m["font"] = s(t.font_family);
    char buf[16];
    if (sp_theme_semantic(&t, SP_SEMANTIC_GO, buf, sizeof buf) == SP_OK)
        m["go"] = QString::fromUtf8(buf);
    if (sp_theme_semantic(&t, SP_SEMANTIC_STOP, buf, sizeof buf) == SP_OK)
        m["stop"] = QString::fromUtf8(buf);
    return m;
}

QVariantMap CoreBridge::theme() const {
    if (!m_haveTheme)
        return QVariantMap();
    return themeMap(m_theme);
}

QVariantMap CoreBridge::previewTheme(int preset, const QString &accent,
                                     const QString &font, int scale) const {
    const QByteArray a = accent.trimmed().toUtf8(), f = font.trimmed().toUtf8();
    sp_theme t {};
    /* Needs no core: resolution is pure, which is why a preview can
       be shown before anything is stored. */
    if (sp_theme_preview((sp_theme_preset)preset,
                         a.isEmpty() ? nullptr : a.constData(),
                         f.isEmpty() ? nullptr : f.constData(),
                         (sp_text_scale)scale, &t) != SP_OK)
        return QVariantMap();
    return themeMap(t);
}

QString CoreBridge::applyTheme(int preset, const QString &accent,
                               const QString &font, int scale) {
    if (!m_core)
        return tr("The core is not running.");
    const QByteArray a = accent.trimmed().toUtf8(), f = font.trimmed().toUtf8();
    const sp_status st =
        sp_theme_set(m_core, (sp_theme_preset)preset,
                     a.isEmpty() ? nullptr : a.constData(),
                     f.isEmpty() ? nullptr : f.constData(),
                     (sp_text_scale)scale);
    if (st != SP_OK)
        return failed(st);
    reloadTheme();
    return QString();
}

int CoreBridge::textScale() const {
    return m_haveTheme ? (int)m_theme.text_scale : (int)SP_TEXT_MEDIUM;
}

QVariantMap CoreBridge::settings() const {
    QVariantMap m;
    sp_settings s {};
    if (!m_core || sp_core_get_settings(m_core, &s) != SP_OK)
        return m;
    m["sipLog"] = s.sip_log;
    m["sipLogRedact"] = s.sip_log_redact;
    m["sipLogMax"] = s.sip_log_max;
    m["logLevel"] = s.log_level;
    m["sipPort"] = s.sip_port;
    return m;
}

bool CoreBridge::showCallLog() const {
    return m_core && sp_ui_get_show_call_log(m_core);
}

void CoreBridge::setShowCallLog(bool on) {
    if (m_core)
        sp_ui_set_show_call_log(m_core, on);
}

bool CoreBridge::showActivityLog() const {
    return m_core && sp_ui_get_show_activity_log(m_core);
}

void CoreBridge::setShowActivityLog(bool on) {
    if (m_core)
        sp_ui_set_show_activity_log(m_core, on);
}

QVariantList CoreBridge::codecs() const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_codec_params *rows = nullptr;
    size_t n = 0;
    if (sp_media_get_codecs(m_core, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["id"] = QString::fromUtf8(rows[i].id ? rows[i].id : "");
        m["enabled"] = rows[i].enabled;
        m["priority"] = rows[i].priority;
        m["clockRate"] = rows[i].clock_rate;
        m["channels"] = rows[i].channel_cnt;
        m["bitrate"] = rows[i].bitrate;
        m["complexity"] = rows[i].complexity;
        m["loss"] = rows[i].expected_loss_pct;
        m["ptime"] = rows[i].ptime_ms;
        m["cbr"] = rows[i].cbr;
        m["framePtime"] = rows[i].frame_ptime_ms;
        /* Read from the media stack rather than guessed from the
           name, so the pane greys out a control instead of offering
           one the codec ignores. */
        m["supportsBitrate"] = rows[i].supports_bitrate;
        m["supportsComplexity"] = rows[i].supports_complexity;
        m["supportsLoss"] = rows[i].supports_expected_loss;
        m["supportsPtime"] = rows[i].supports_ptime;
        m["supportsCbr"] = rows[i].supports_cbr;
        out.append(m);
    }
    return out;
}

/* The core applies a list, not a single flag: the order is part of
   what is being set, so every change reads the whole set, edits one
   row and hands the set back. The ids have to be copied on the way --
   the core's storage is good only until its next call, and
   sp_media_set_codecs is a call. */
namespace {
struct CodecSet {
    QList<QByteArray> ids;
    QList<sp_codec_params> rows;

    bool read(sp_core *core) {
        const sp_codec_params *v = nullptr;
        size_t n = 0;
        if (sp_media_get_codecs(core, &v, &n) != SP_OK)
            return false;
        for (size_t i = 0; i < n; i++) {
            ids.append(QByteArray(v[i].id ? v[i].id : ""));
            rows.append(v[i]);
        }
        return true;
    }
    int indexOf(const QString &id) const {
        for (int i = 0; i < ids.size(); i++)
            if (QString::fromUtf8(ids[i]) == id)
                return i;
        return -1;
    }
    sp_status apply(sp_core *core) {
        for (int i = 0; i < rows.size(); i++)
            rows[i].id = ids[i].constData();
        return sp_media_set_codecs(core, rows.constData(),
                                   (size_t)rows.size());
    }
};
}  // namespace

QString CoreBridge::applyCodecs(void *set) {
    CodecSet *s = static_cast<CodecSet *>(set);
    const sp_status st = s->apply(m_core);
    if (st == SP_OK)
        return QString();
    return QStringLiteral("%1 %2")
        .arg(QString::fromUtf8(sp_strerror(st)),
             QString::fromUtf8(sp_last_error_detail(m_core)))
        .trimmed();
}

QString CoreBridge::setCodecEnabled(const QString &id, bool on) {
    if (!m_core)
        return tr("The core is not running.");
    CodecSet set;
    if (!set.read(m_core))
        return tr("The codec list could not be read.");
    const int i = set.indexOf(id);
    if (i < 0)
        return tr("No such codec.");
    set.rows[i].enabled = on;
    return applyCodecs(&set);
}

QString CoreBridge::setCodecParam(const QString &id, const QString &key,
                                  int value) {
    if (!m_core)
        return tr("The core is not running.");
    CodecSet set;
    if (!set.read(m_core))
        return tr("The codec list could not be read.");
    const int i = set.indexOf(id);
    if (i < 0)
        return tr("No such codec.");
    sp_codec_params &p = set.rows[i];
    if (key == QLatin1String("bitrate"))
        p.bitrate = value;
    else if (key == QLatin1String("complexity"))
        p.complexity = value;
    else if (key == QLatin1String("loss"))
        p.expected_loss_pct = value;
    else if (key == QLatin1String("ptime"))
        p.ptime_ms = value;
    else if (key == QLatin1String("cbr"))
        p.cbr = value;
    else
        return tr("No such codec setting.");
    return applyCodecs(&set);
}

QString CoreBridge::moveCodec(const QString &id, int delta) {
    if (!m_core)
        return tr("The core is not running.");
    CodecSet set;
    if (!set.read(m_core))
        return tr("The codec list could not be read.");
    const int i = set.indexOf(id);
    if (i < 0)
        return tr("No such codec.");
    const int to = i + delta;
    /* Off either end is nothing to do rather than a failure: the
       button at the top of the list is pressed, not disabled. */
    if (to < 0 || to >= set.rows.size())
        return QString();
    set.rows.move(i, to);
    set.ids.move(i, to);
    return applyCodecs(&set);
}

QString CoreBridge::applyCodecList(const QVariantList &rows) {
    if (!m_core)
        return tr("The core is not running.");
    CodecSet current;
    if (!current.read(m_core))
        return tr("The codec list could not be read.");

    /* The page's whole working copy, in the order it was left in --
       reordering and enabling are edits on that copy, not on the
       core, until this one call. A row the core no longer knows is
       dropped rather than resurrected. */
    CodecSet next;
    for (const QVariant &rv : rows) {
        const QVariantMap m = rv.toMap();
        const int i = current.indexOf(m.value(QStringLiteral("id")).toString());
        if (i < 0)
            continue;
        sp_codec_params p = current.rows[i];
        p.enabled = m.value(QStringLiteral("enabled")).toBool();
        p.bitrate = m.value(QStringLiteral("bitrate")).toInt();
        p.complexity = m.value(QStringLiteral("complexity")).toInt();
        p.expected_loss_pct = m.value(QStringLiteral("loss")).toInt();
        p.ptime_ms = m.value(QStringLiteral("ptime")).toInt();
        p.cbr = m.value(QStringLiteral("cbr")).toInt();
        next.ids.append(current.ids[i]);
        next.rows.append(p);
    }
    return applyCodecs(&next);
}

QVariantList CoreBridge::modules() const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_module *rows = nullptr;
    size_t n = 0;
    if (sp_modules_get(m_core, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["id"] = QString::fromUtf8(rows[i].id ? rows[i].id : "");
        m["label"] = QString::fromUtf8(rows[i].label ? rows[i].label : "");
        m["enabled"] = rows[i].enabled;
        m["removable"] = rows[i].removable;
        out.append(m);
    }
    return out;
}

QString CoreBridge::setModule(const QString &id, bool on) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_module_set(m_core, id.toUtf8().constData(), on));
}

QVariantList CoreBridge::transcript(qlonglong historyId) const {
    QVariantList out;
    const sp_transcript_line *l = nullptr;
    size_t n = 0;
    if (!m_core || historyId <= 0
        || sp_transcript_read(m_core, (int64_t)historyId, &l, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["startMs"] = (qlonglong)l[i].start_ms;
        m["endMs"] = (qlonglong)l[i].end_ms;
        m["local"] = l[i].local;
        m["speaker"] = QString::fromUtf8(l[i].speaker ? l[i].speaker : "");
        m["text"] = QString::fromUtf8(l[i].text ? l[i].text : "");
        out.append(m);
    }
    return out;
}

QString CoreBridge::transcriptText(qlonglong historyId) const {
    QStringList lines;
    for (const QVariant &v : transcript(historyId)) {
        const QVariantMap m = v.toMap();
        const qlonglong s = m["startMs"].toLongLong() / 1000;
        lines << QStringLiteral("%1:%2  %3: %4")
                     .arg(s / 60, 2, 10, QLatin1Char('0'))
                     .arg(s % 60, 2, 10, QLatin1Char('0'))
                     .arg(m["speaker"].toString(), m["text"].toString());
    }
    return lines.join(QLatin1Char('\n'));
}

bool CoreBridge::transcriptAvailable() const {
    return m_core && sp_transcript_available(m_core);
}

QString CoreBridge::transcriptModel() const {
    char p[4096];
    if (!m_core || sp_transcript_get_model(m_core, p, sizeof p) != SP_OK)
        return QString();
    return QString::fromUtf8(p);
}

QString CoreBridge::setTranscriptModel(const QString &path) {
    if (!m_core)
        return tr("The core is not running.");
    const QByteArray p = path.toUtf8();
    return failed(sp_transcript_set_model(m_core,
                                          p.isEmpty() ? nullptr : p.constData()));
}

QVariantMap CoreBridge::levels() const {
    QVariantMap m;
    sp_audio_levels l {};
    if (!m_core || sp_media_get_levels(m_core, &l) != SP_OK) {
        m["tx"] = 1.0;
        m["rx"] = 1.0;
        m["txMuted"] = false;
        m["rxMuted"] = false;
        return m;
    }
    m["tx"] = (qreal)l.tx_level;
    m["rx"] = (qreal)l.rx_level;
    m["txMuted"] = l.tx_muted;
    m["rxMuted"] = l.rx_muted;
    return m;
}

/* A level outside 0.0..2.0 is refused by the core rather than clamped,
   so that a frontend bug is visible instead of quietly halving the
   volume. This passes the refusal on for the same reason. */
bool CoreBridge::setLevels(qreal tx, qreal rx, bool txMuted, bool rxMuted) {
    if (!m_core)
        return false;
    sp_audio_levels l {};
    l.tx_level = (float)tx;
    l.rx_level = (float)rx;
    l.tx_muted = txMuted;
    l.rx_muted = rxMuted;
    return sp_media_set_levels(m_core, &l) == SP_OK;
}

QVariantMap CoreBridge::dsp() const {
    QVariantMap m;
    sp_audio_dsp d {};
    if (!m_core || sp_media_get_dsp(m_core, &d) != SP_OK)
        return m;
    m["echoCancel"] = d.echo_cancel;
    m["echoTailMs"] = d.echo_tail_ms;
    m["noiseSuppression"] = d.noise_suppression;
    m["autoGain"] = d.auto_gain;
    m["silenceSuppression"] = d.silence_suppression;
    m["comfortNoise"] = d.comfort_noise;
    m["packetLossConcealment"] = d.packet_loss_concealment;
    return m;
}

void CoreBridge::setDsp(const QString &key, bool on) {
    sp_audio_dsp d {};
    if (!m_core || sp_media_get_dsp(m_core, &d) != SP_OK)
        return;
    if (key == "echoCancel") d.echo_cancel = on;
    else if (key == "noiseSuppression") d.noise_suppression = on;
    else if (key == "autoGain") d.auto_gain = on;
    else if (key == "silenceSuppression") d.silence_suppression = on;
    else if (key == "comfortNoise") d.comfort_noise = on;
    else if (key == "packetLossConcealment") d.packet_loss_concealment = on;
    else return;
    sp_media_set_dsp(m_core, &d);
}

void CoreBridge::setEchoTailMs(int ms) {
    sp_audio_dsp d {};
    if (!m_core || sp_media_get_dsp(m_core, &d) != SP_OK)
        return;
    d.echo_tail_ms = ms;
    sp_media_set_dsp(m_core, &d);
}

/* One shape for every call that either works or says why. */
QString CoreBridge::failed(sp_status st) const {
    if (st == SP_OK)
        return QString();
    return QStringLiteral("%1 %2")
        .arg(QString::fromUtf8(sp_strerror(st)),
             QString::fromUtf8(sp_last_error_detail(m_core)))
        .trimmed();
}

/* A native folder dialog hands back a "file://" URL rather than a
   plain path; passed straight through when it is a plain path
   already, the same as importSound's url argument. */
static QString localPath(const QString &urlOrPath) {
    const QUrl u(urlOrPath);
    return u.isLocalFile() ? u.toLocalFile() : urlOrPath;
}

QString CoreBridge::setRecordingDir(const QString &dir) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_recording_set_dir(
        m_core, localPath(dir).toUtf8().constData()));
}

QString CoreBridge::setTraceDir(const QString &dir) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_trace_set_export_dir(
        m_core, localPath(dir).toUtf8().constData()));
}

namespace {
/* Converts anything that is not a WAV, since the media stack has no
   other decoder -- the same conversion the Qt window's Sounds page
   ran through QProcess before ever calling sp_sound_import. Returns
   an empty string and sets *whyNot on failure. */
QString convertToWav(const QString &src, QString *whyNot) {
    const QString ff =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ff.isEmpty()) {
        *whyNot = CoreBridge::tr(
            "This is not a WAV file, and ffmpeg is not installed to "
            "convert it. Install ffmpeg, or convert it yourself to "
            "16-bit PCM WAV.");
        return QString();
    }

    /* Converted inside the application's own configuration directory
       rather than the system temp: /tmp is world-readable and space
       limited, and this file is the operator's audio. Handed to the
       core afterwards, which copies it into place and proves the
       media stack can read it before keeping it. */
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString out = QDir(dir).filePath(
        QStringLiteral(".import-") + QFileInfo(src).completeBaseName() +
        QStringLiteral(".wav"));

    QProcess p;
    /* 16 kHz mono 16-bit, matching the bundled sounds and the
       conference bridge's own rate. */
    p.start(ff, {QStringLiteral("-loglevel"), QStringLiteral("error"),
                 QStringLiteral("-y"), QStringLiteral("-i"), src,
                 QStringLiteral("-ac"), QStringLiteral("1"),
                 QStringLiteral("-ar"), QStringLiteral("16000"),
                 QStringLiteral("-acodec"), QStringLiteral("pcm_s16le"),
                 out});
    if (!p.waitForFinished(30000) || p.exitCode() != 0) {
        *whyNot = CoreBridge::tr("ffmpeg could not convert it: %1")
                      .arg(QString::fromUtf8(p.readAllStandardError())
                               .trimmed());
        QFile::remove(out);
        return QString();
    }
    return out;
}
}  // namespace

QUrl CoreBridge::musicFolder() const {
    return QUrl::fromLocalFile(
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
}

QString CoreBridge::importSound(int slot, const QString &url) {
    if (!m_core)
        return tr("The core is not running.");
    const QUrl u(url);
    const QString src = u.isLocalFile() ? u.toLocalFile() : url;

    QString use = src;
    QString converted;
    if (QFileInfo(src).suffix().compare(QLatin1String("wav"),
                                        Qt::CaseInsensitive) != 0) {
        QString why;
        converted = convertToWav(src, &why);
        if (converted.isEmpty())
            return why;
        use = converted;
    }

    const char *stored = nullptr;
    const sp_status st = sp_sound_import(m_core, (sp_sound_slot)slot,
                                         use.toUtf8().constData(), &stored);
    if (!converted.isEmpty())
        QFile::remove(converted);
    if (st != SP_OK)
        return failed(st);
    /* Imported and then selected: importing a ringtone and having to
       pick it from a list afterwards is a step nobody wants. Selected
       by name rather than by the stored path, the same as the Qt
       window's combo ends up doing -- sp_sound_list identifies an
       imported sound by name, and soundInUse has to answer with
       something the Sounds page's own model recognises. */
    const QString name =
        QFileInfo(QString::fromUtf8(stored)).completeBaseName();
    sp_sound_set(m_core, (sp_sound_slot)slot, name.toUtf8().constData());
    return QString();
}

QStringList CoreBridge::headerNames() const {
    QStringList out;
    if (!m_core)
        return out;
    const char **names = nullptr;
    size_t n = 0;
    if (sp_header_names_list(m_core, &names, &n) != SP_OK || !names)
        return out;
    for (size_t i = 0; i < n; i++)
        out << QString::fromUtf8(names[i] ? names[i] : "");
    return out;
}

bool CoreBridge::rememberHeaderName(const QString &name) {
    return m_core
        && sp_header_names_remember(m_core, name.toUtf8().constData())
               == SP_OK;
}

bool CoreBridge::logAction(int level, const QString &text) {
    return m_core
        && sp_actionlog_add(m_core, level, text.toUtf8().constData())
               == SP_OK;
}

int CoreBridge::logOrder() const {
    return m_core ? (int)sp_log_get_order(m_core) : 0;
}

void CoreBridge::setLogOrder(int order) {
    if (m_core)
        sp_log_set_order(m_core, (sp_order)order);
}

bool CoreBridge::clearLog(const QString &which) {
    if (!m_core)
        return false;
    if (which == QLatin1String("sip"))
        return sp_siplog_clear(m_core) == SP_OK;
    if (which == QLatin1String("history"))
        return sp_history_clear(m_core) == SP_OK;
    if (which == QLatin1String("activity"))
        return sp_actionlog_clear(m_core) == SP_OK;
    return false;
}

QVariantMap CoreBridge::recordingSettings() const {
    QVariantMap m;
    if (!m_core)
        return m;
    const char *dir = nullptr;
    sp_recording_format fmt = SP_REC_WAV;
    if (sp_recording_get_dir(m_core, &dir) == SP_OK)
        m["dir"] = QString::fromUtf8(dir ? dir : "");
    if (sp_recording_get_format(m_core, &fmt) == SP_OK)
        m["format"] = (int)fmt;
    const char *trace = nullptr;
    if (sp_trace_get_export_dir(m_core, &trace) == SP_OK)
        m["traceDir"] = QString::fromUtf8(trace ? trace : "");
    m["auto"] = sp_recording_get_auto(m_core);
    return m;
}

QString CoreBridge::setRecordingAuto(bool on) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_recording_set_auto(m_core, on));
}

QString CoreBridge::setRecordingFormat(int fmt) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_recording_set_format(m_core, (sp_recording_format)fmt));
}

QVariantList CoreBridge::siplog(const QString &callId, int limit) const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_sip_message *rows = nullptr;
    size_t n = 0;
    const QByteArray c = callId.toUtf8();
    if (sp_siplog_read(m_core, c.isEmpty() ? nullptr : c.constData(),
                       (size_t)limit, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["index"] = (int)i;
        m["tsUs"] = (qulonglong)rows[i].ts_us;
        m["direction"] = QString::fromUtf8(rows[i].direction
                                               ? rows[i].direction : "");
        m["method"] = QString::fromUtf8(rows[i].method ? rows[i].method : "");
        m["status"] = rows[i].status_code;
        m["callId"] = QString::fromUtf8(rows[i].call_id ? rows[i].call_id
                                                        : "");
        m["redacted"] = rows[i].redacted;
        out.append(m);
    }
    return out;
}

/* The full text of one message, fetched only when it is opened: the
   list holds thousands and the raw of each is a kilobyte. */
QString CoreBridge::sipMessageAt(const QString &callId, int index) const {
    if (!m_core)
        return QString();
    const sp_sip_message *rows = nullptr;
    size_t n = 0;
    const QByteArray c = callId.toUtf8();
    if (sp_siplog_read(m_core, c.isEmpty() ? nullptr : c.constData(),
                       (size_t)index + 1, &rows, &n) != SP_OK
        || (size_t)index >= n)
        return QString();
    return QString::fromUtf8(rows[index].raw ? rows[index].raw : "");
}

bool CoreBridge::playbackStart(const QString &path) {
    return m_core
        && sp_playback_start(m_core, path.toUtf8().constData()) == SP_OK;
}
void CoreBridge::playbackPause()  { if (m_core) sp_playback_pause(m_core); }
void CoreBridge::playbackResume() { if (m_core) sp_playback_resume(m_core); }
void CoreBridge::playbackStop()   { if (m_core) sp_playback_stop(m_core); }
void CoreBridge::playbackSeek(int positionMs) {
    if (m_core) sp_playback_seek(m_core, (uint32_t)positionMs);
}
void CoreBridge::playbackSpeed(qreal speed) {
    if (m_core) sp_playback_set_speed(m_core, (float)speed);
}

/* Copied to a temporary beside the destination and renamed into
   place, so dest is always either the old file or the new one: the
   Qt window's saveRecordingCopy, for the same hazard. */
QString CoreBridge::saveRecordingCopy(const QString &src,
                                      const QString &destUrl) {
    const QUrl u(destUrl);
    const QString dest = u.isLocalFile() ? u.toLocalFile() : destUrl;
    if (src.isEmpty() || dest.isEmpty())
        return QString();
    const QString tmp = dest + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!QFile::copy(src, tmp)) {
        QFile::remove(tmp);
        const QString msg = tr("could not save a copy of %1 to %2")
                                .arg(src, dest);
        note(msg);
        return msg;
    }
    if (::rename(tmp.toLocal8Bit().constData(),
                 dest.toLocal8Bit().constData()) != 0) {
        const QString msg = tr("could not replace %1: %2")
                                .arg(dest, QString::fromUtf8(strerror(errno)));
        QFile::remove(tmp);
        note(msg);
        return msg;
    }
    return QString();
}

QVariantMap CoreBridge::playback() const {
    QVariantMap m;
    sp_playback p {};
    if (!m_core || sp_playback_get(m_core, &p) != SP_OK) {
        m["state"] = 0;
        return m;
    }
    m["state"] = (int)p.state;
    m["positionMs"] = (int)p.position_ms;
    m["durationMs"] = (int)p.duration_ms;
    m["path"] = QString::fromUtf8(p.path ? p.path : "");
    /* The core says whether seeking is possible -- it refuses on an
       OggOpus recording -- rather than the frontend guessing from an
       extension. */
    m["seekable"] = p.seekable;
    m["speed"] = p.speed;
    return m;
}

QVariantMap CoreBridge::contact(qlonglong id) const {
    QVariantMap m;
    m["id"] = (qlonglong)0;
    m["name"] = QString();
    m["company"] = QString();
    m["note"] = QString();
    m["favourite"] = false;
    m["numbers"] = QStringList();
    if (!m_core || id == 0)
        return m;
    const sp_contact *c = nullptr;
    if (sp_contact_read_one(m_core, (int64_t)id, &c) != SP_OK || !c)
        return m;
    m["id"] = (qlonglong)c->id;
    m["name"] = QString::fromUtf8(c->name ? c->name : "");
    m["company"] = QString::fromUtf8(c->company ? c->company : "");
    m["note"] = QString::fromUtf8(c->note ? c->note : "");
    m["favourite"] = c->favourite;
    QStringList nums;
    for (size_t i = 0; i < c->n_numbers; i++)
        nums << QStringLiteral("%1|%2")
                    .arg(QString::fromUtf8(c->numbers[i].label
                                               ? c->numbers[i].label : ""),
                         QString::fromUtf8(c->numbers[i].uri
                                               ? c->numbers[i].uri : ""));
    m["numbers"] = nums;
    return m;
}

qlonglong CoreBridge::saveContact(qlonglong id, const QString &name,
                                  const QString &company, const QString &note,
                                  bool favourite, const QStringList &numbers) {
    if (!m_core || name.trimmed().isEmpty())
        return 0;
    /* The byte arrays have to outlive the call, so they are held here
       rather than built inside the loop that points at them. */
    QList<QByteArray> keep;
    QList<sp_contact_number> nums;
    for (const QString &n : numbers) {
        const int bar = n.indexOf(QLatin1Char('|'));
        if (bar < 0)
            continue;
        const QString uri = n.mid(bar + 1).trimmed();
        if (uri.isEmpty())
            continue;
        keep.append(n.left(bar).toUtf8());
        keep.append(uri.toUtf8());
        sp_contact_number cn {};
        cn.id = 0;
        cn.label = keep[keep.size() - 2].constData();
        cn.uri = keep[keep.size() - 1].constData();
        nums.append(cn);
    }
    const QByteArray nm = name.toUtf8(), co = company.toUtf8(),
                     nt = note.toUtf8();
    sp_contact c {};
    c.id = (int64_t)id;
    c.name = nm.constData();
    c.company = co.isEmpty() ? nullptr : co.constData();
    c.note = nt.isEmpty() ? nullptr : nt.constData();
    c.favourite = favourite;
    c.numbers = nums.constData();
    c.n_numbers = (size_t)nums.size();
    /* The core replaces a contact whole, so a save that says nothing
       about the picture is a save that deletes it. Editing a name
       must not cost the operator the picture. */
    QByteArray keepPicture;
    if (id != 0) {
        const void *data = nullptr;
        size_t len = 0;
        const char *type = nullptr;
        if (sp_contact_picture(m_core, (int64_t)id, &data, &len, &type)
                == SP_OK && data && len > 0) {
            keepPicture = QByteArray(static_cast<const char *>(data),
                                     (int)len);
            c.picture = keepPicture.constData();
            c.picture_len = (size_t)keepPicture.size();
            c.picture_type = "png";
        }
    }
    int64_t out = 0;
    const sp_status st = sp_contact_save(m_core, &c, &out);
    if (st != SP_OK) {
        /* contacteditor.cpp shows "Could not save: %1" with this same
           detail; the QML editor reads it back through lastError
           rather than a second return channel. */
        m_lastError = failed(st);
        emit lastErrorChanged();
        return 0;
    }
    return (qlonglong)out;
}

/* Choosing a picture.
 *
 * The numbers are the Widgets dialog's, kept identical so a picture
 * accepted on one platform is accepted on the other. They bound
 * decoding, which is where a small file becomes a large allocation;
 * the size of what is *stored* is the core's rule and it enforces it
 * whatever happens here. */
namespace {
constexpr qint64 MAX_PICTURE_FILE_BYTES = 20LL * 1024 * 1024;
constexpr qint64 MAX_PICTURE_PIXELS     = 80LL * 1000 * 1000;
constexpr int    PICTURE_EDGE           = 256;
constexpr int    MAX_PICTURE_BYTES      = 512 * 1024;
/* Some formats' headers carry no size, so the decode itself is
   bounded instead of trusting Qt's default ceiling. The same
   arithmetic the Widgets dialog does. */
constexpr int MAX_DECODE_MIB =
    (int)((MAX_PICTURE_PIXELS * 4 + (1024 * 1024 - 1)) / (1024 * 1024));
constexpr int MIN_CROP_EDGE = 16;

QRect clampSquare(const QSize &size, QRect r) {
    if (size.isEmpty())
        return QRect();
    const int maxEdge = qMin(size.width(), size.height());
    if (r.isNull() || r.width() <= 0)
        r = QRect((size.width() - maxEdge) / 2, (size.height() - maxEdge) / 2,
                  maxEdge, maxEdge);
    /* The floor gives way on an image smaller than it: qBound with a
       lower bound above its upper one asserts in a debug Qt and
       silently returns the larger in a release one, which QImage::copy
       then pads with black rather than refusing. */
    const int minEdge = qMin(MIN_CROP_EDGE, maxEdge);
    const int edge = qBound(minEdge, r.width(), maxEdge);
    const int left = qBound(0, r.left(), size.width() - edge);
    const int top = qBound(0, r.top(), size.height() - edge);
    return QRect(left, top, edge, edge);
}
}  // namespace

QString CoreBridge::stagePicture(const QString &url) {
    m_staged = QImage();
    /* content:// on Android, file:// on the desktop. QFile opens both;
       a bare path is taken as a path. */
    const QUrl u(url);
    const QString path = u.isLocalFile() ? u.toLocalFile()
                                         : (u.scheme().isEmpty() ? url : url);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tr("That file could not be opened.");
    if (file.size() > MAX_PICTURE_FILE_BYTES)
        return tr("That file is %1 MB. The limit is 20 MB.")
            .arg(file.size() / (1024 * 1024));

    QImageReader reader(&file);
    reader.setAutoTransform(true);
    const QSize declared = reader.size();
    if (declared.isValid()) {
        const qint64 pixels = (qint64)declared.width() * declared.height();
        if (pixels > MAX_PICTURE_PIXELS)
            return tr("That picture is %1 megapixels. The limit is 80.")
                .arg(pixels / 1000000);
    } else {
        reader.setAllocationLimit(MAX_DECODE_MIB);
    }

    const QImage img = reader.read();
    if (img.isNull())
        return reader.errorString().isEmpty()
                   ? tr("That file is not a picture.")
                   : reader.errorString();
    m_staged = img;
    return QString();
}

QVariantMap CoreBridge::stagedPicture() const {
    QVariantMap m;
    m["width"] = m_staged.width();
    m["height"] = m_staged.height();
    return m;
}

QByteArray CoreBridge::encodeStagedCrop(int x, int y, int edge,
                                        QString *errorOut) const {
    if (errorOut) errorOut->clear();
    if (m_staged.isNull()) {
        if (errorOut) *errorOut = tr("No picture has been chosen.");
        return QByteArray();
    }
    const QRect r = clampSquare(m_staged.size(), QRect(x, y, edge, edge));
    const QImage square =
        m_staged.copy(r).scaled(PICTURE_EDGE, PICTURE_EDGE,
                                Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!square.save(&buf, "PNG")) {
        if (errorOut) *errorOut = tr("That picture could not be encoded.");
        return QByteArray();
    }
    if (png.size() > MAX_PICTURE_BYTES) {
        if (errorOut)
            *errorOut = tr("The encoded picture is %1 KB. The limit is "
                           "512 KB.").arg(png.size() / 1024);
        return QByteArray();
    }
    return png;
}

QString CoreBridge::commitPicture(qlonglong contactId, int x, int y,
                                  int edge) {
    QString err;
    const QByteArray png = encodeStagedCrop(x, y, edge, &err);
    if (png.isEmpty() && !err.isEmpty())
        return err;
    return rewritePicture(contactId, png);
}

QString CoreBridge::clearPicture(qlonglong contactId) {
    return rewritePicture(contactId, QByteArray());
}

QString CoreBridge::stagedPreview(int x, int y, int edge) const {
    QString err;
    const QByteArray png = encodeStagedCrop(x, y, edge, &err);
    if (png.isEmpty())
        return QString();
    return QStringLiteral("data:image/png;base64,")
        + QString::fromLatin1(png.toBase64());
}

QStringList CoreBridge::pictureFormats() const {
    QStringList out;
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        out << QString::fromLatin1(f).toLower();
    out.removeDuplicates();
    out.sort();
    return out;
}

/* The core replaces a contact whole, so a picture is written by
   saving the contact it belongs to again. Everything else has to be
   carried across unchanged or a new picture would cost the operator
   the note they had typed. */
QString CoreBridge::rewritePicture(qlonglong contactId,
                                   const QByteArray &png) {
    if (!m_core || contactId == 0)
        return tr("There is no contact to attach it to.");
    const sp_contact *c = nullptr;
    if (sp_contact_read_one(m_core, (int64_t)contactId, &c) != SP_OK || !c)
        return tr("That contact no longer exists.");

    sp_contact out = *c;
    out.picture = png.isEmpty() ? nullptr : png.constData();
    out.picture_len = (size_t)png.size();
    out.picture_type = png.isEmpty() ? nullptr : "png";
    int64_t id = 0;
    const sp_status st = sp_contact_save(m_core, &out, &id);
    if (st != SP_OK)
        return QStringLiteral("%1 %2")
            .arg(QString::fromUtf8(sp_strerror(st)),
                 QString::fromUtf8(sp_last_error_detail(m_core)))
            .trimmed();
    return QString();
}

QVariantMap CoreBridge::whoIs(const QString &uri) const {
    QVariantMap m;
    if (!m_core || uri.trimmed().isEmpty())
        return m;
    const sp_contact *c = nullptr;
    if (sp_contact_lookup(m_core, uri.toUtf8().constData(), &c) != SP_OK || !c)
        return m;
    m["id"] = (qlonglong)c->id;
    m["name"] = QString::fromUtf8(c->name ? c->name : "");
    m["company"] = QString::fromUtf8(c->company ? c->company : "");
    m["favourite"] = c->favourite;
    return m;
}

/* The one event that has to interrupt. The ongoing "Reachable"
   notification deliberately never changes -- one that changed with
   every event would be one nobody reads -- so an incoming call gets
   its own, on its own channel, at high importance. */
void CoreBridge::announceIncoming(const QString &who) {
#ifdef Q_OS_ANDROID
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return;
    QJniObject::callStaticMethod<void>(
        "io/github/etienne_gn/comlink/Notifier", "incoming",
        "(Landroid/content/Context;Ljava/lang/String;)V",
        ctx.object(), QJniObject::fromString(who).object<jstring>());
#else
    Q_UNUSED(who);
#endif
}

void CoreBridge::clearIncoming() {
#ifdef Q_OS_ANDROID
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return;
    QJniObject::callStaticMethod<void>(
        "io/github/etienne_gn/comlink/Notifier", "clearIncoming",
        "(Landroid/content/Context;)V", ctx.object());
#endif
}

bool CoreBridge::windowButtonsOnLeft() const {
#ifdef Q_OS_ANDROID
    return false;
#else
    /* Asked once. gsettings is a process launch, and this is read
       every time the title bar lays itself out. */
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    cached = 0;

    QProcess p;
    p.start(QStringLiteral("gsettings"),
            {QStringLiteral("get"),
             QStringLiteral("org.gnome.desktop.wm.preferences"),
             QStringLiteral("button-layout")});
    if (!p.waitForFinished(1500) || p.exitCode() != 0)
        return false;

    /* The value is "close,minimize,maximize:appmenu": what precedes
       the colon sits on the left. Anything unreadable falls back to
       the right, which is the wider convention.
     *
     * Note for anyone reading a screenshot: every probe and test here
     * redirects XDG_CONFIG_HOME, and dconf keeps its user database
     * under it -- so gsettings answers with the *default* layout in a
     * test run and the buttons come out on the right. That is the
     * harness, not this. */
    const QString layout = QString::fromUtf8(p.readAllStandardOutput());
    const int colon = layout.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return false;
    const QString left = layout.left(colon);
    cached = (left.contains(QStringLiteral("close"))
              || left.contains(QStringLiteral("minimize"))
              || left.contains(QStringLiteral("maximize"))) ? 1 : 0;
    return cached != 0;
#endif
}

/* Qt's activity finishes on Back, which takes the process -- and the
   registration, and the foreground service holding it up -- with it.
   A phone goes to the background instead; the notification is how it
   is brought back. */
void CoreBridge::moveToBackground() {
#ifdef Q_OS_ANDROID
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid())
        activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", JNI_TRUE);
#endif
}

bool CoreBridge::networkChanged() {
    return m_core && sp_core_network_changed(m_core) == SP_OK;
}

bool CoreBridge::deleteContact(qlonglong id) {
    return m_core && sp_contact_delete(m_core, (int64_t)id) == SP_OK;
}

QVariantList CoreBridge::audioDevices() const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_audio_device *rows = nullptr;
    size_t n = 0;
    if (sp_media_enum_audio_devices(m_core, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(rows[i].name ? rows[i].name : "");
        m["label"] = QString::fromUtf8(rows[i].label ? rows[i].label : "");
        m["driver"] = QString::fromUtf8(rows[i].driver ? rows[i].driver : "");
        m["rate"] = (int)rows[i].default_rate_hz;
        m["inputs"] = (int)rows[i].input_count;
        m["outputs"] = (int)rows[i].output_count;
        /* SP_AUDIO_RAW and SP_AUDIO_PLUGIN, by their fixed ordinal in
           the ABI's enum -- a real card with no conversion, or the
           rate converters and channel maps ALSA wraps around one --
           which is what the Audio page suffixes onto the label. */
        m["kind"] = (int)rows[i].kind;
        /* The core says which entries are worth offering -- the
           default, the sound servers, one usable entry per card --
           and a frontend filtering the list itself would disagree
           with the other platform about which card works at 16 kHz. */
        m["recommended"] = rows[i].recommended;
        out.append(m);
    }
    return out;
}

QVariantMap CoreBridge::audioDevicesInUse() const {
    QVariantMap m;
    const char *cap = nullptr, *play = nullptr;
    if (m_core && sp_media_get_audio_devices(m_core, &cap, &play) == SP_OK) {
        m["capture"] = QString::fromUtf8(cap ? cap : "");
        m["playback"] = QString::fromUtf8(play ? play : "");
    }
    return m;
}

QString CoreBridge::setAudioDevices(const QString &capture,
                                    const QString &playback) {
    if (!m_core)
        return tr("The core is not running.");
    const QByteArray c = capture.toUtf8(), p = playback.toUtf8();
    return failed(sp_media_set_audio_devices(m_core, c.constData(),
                                             p.constData()));
}

QVariantList CoreBridge::soundChoices(int slot) const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_sound_file *rows = nullptr;
    size_t n = 0;
    if (sp_sound_list(m_core, (sp_sound_slot)slot, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(rows[i].name ? rows[i].name : "");
        m["path"] = QString::fromUtf8(rows[i].path ? rows[i].path : "");
        m["bundled"] = rows[i].bundled;
        out.append(m);
    }
    return out;
}

QString CoreBridge::soundInUse(int slot) const {
    const char *v = nullptr;
    if (m_core && sp_sound_get(m_core, (sp_sound_slot)slot, &v) == SP_OK)
        return QString::fromUtf8(v ? v : "");
    return QString();
}

QString CoreBridge::setSound(int slot, const QString &value) {
    if (!m_core)
        return tr("The core is not running.");
    return failed(sp_sound_set(m_core, (sp_sound_slot)slot,
                               value.toUtf8().constData()));
}

void CoreBridge::previewSound(int slot) {
    if (m_core)
        sp_sound_preview(m_core, (sp_sound_slot)slot);
}

void CoreBridge::previewStop() {
    if (m_core)
        sp_sound_preview_stop(m_core);
}

void CoreBridge::silenceFailureTone() {
    if (m_core)
        sp_sound_stop_notification(m_core);
}

QVariantList CoreBridge::ruleStats() const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_rule_stat *rows = nullptr;
    size_t n = 0;
    if (sp_rules_stats(m_core, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(rows[i].name ? rows[i].name : "");
        m["hits"] = (qulonglong)rows[i].hits;
        out.append(m);
    }
    return out;
}

QVariantList CoreBridge::headerPresets() const {
    QVariantList out;
    if (!m_core)
        return out;
    const sp_header_preset *rows = nullptr;
    size_t n = 0;
    if (sp_presets_list(m_core, &rows, &n) != SP_OK)
        return out;
    for (size_t i = 0; i < n; i++) {
        QVariantMap m;
        m["name"] = QString::fromUtf8(rows[i].name ? rows[i].name : "");
        QStringList hs;
        for (size_t k = 0; k < rows[i].n_headers; k++)
            hs << QStringLiteral("%1|%2")
                      .arg(QString::fromUtf8(rows[i].headers[k].name
                                                 ? rows[i].headers[k].name : ""),
                           QString::fromUtf8(rows[i].headers[k].value
                                                 ? rows[i].headers[k].value : ""));
        m["headers"] = hs;
        out.append(m);
    }
    return out;
}

/* The byte arrays behind sp_header have to outlive the call, so they
   are held in one list rather than built inside the loop. */
static QList<sp_header> buildHeaders(const QStringList &in,
                                     QList<QByteArray> &keep) {
    QList<sp_header> out;
    for (const QString &h : in) {
        const int bar = h.indexOf(QLatin1Char('|'));
        if (bar <= 0)
            continue;
        keep.append(h.left(bar).toUtf8());
        keep.append(h.mid(bar + 1).toUtf8());
        sp_header sh {};
        sh.name = keep[keep.size() - 2].constData();
        sh.value = keep[keep.size() - 1].constData();
        out.append(sh);
    }
    return out;
}

bool CoreBridge::savePreset(const QString &name, const QStringList &headers) {
    if (!m_core || name.trimmed().isEmpty())
        return false;
    QList<QByteArray> keep;
    const QList<sp_header> hs = buildHeaders(headers, keep);
    const sp_status st = sp_presets_save(m_core, name.toUtf8().constData(),
                                         hs.constData(), (size_t)hs.size());
    if (st != SP_OK) {
        /* headersdialog.cpp's own two lines, so the dialog can show
           the same failure the desktop build would. */
        m_lastError = QStringLiteral("%1\n%2")
                          .arg(QString::fromUtf8(sp_strerror(st)),
                               QString::fromUtf8(sp_last_error_detail(m_core)));
        emit lastErrorChanged();
        return false;
    }
    return true;
}

bool CoreBridge::deletePreset(const QString &name) {
    return m_core
        && sp_presets_delete(m_core, name.toUtf8().constData()) == SP_OK;
}

bool CoreBridge::dialWithHeaders(const QString &uri,
                                 const QStringList &headers) {
    if (!m_core || m_account == 0) {
        note(QStringLiteral("no account selected"));
        m_lastError = tr("No account to call from.");
        emit lastErrorChanged();
        emit callChanged();
        return false;
    }
    QList<QByteArray> keep;
    const QList<sp_header> hs = buildHeaders(headers, keep);
    char full[512];
    const sp_status cs = sp_call_uri_complete(m_core, m_account,
                                              uri.toUtf8().constData(),
                                              full, sizeof full);
    if (cs != SP_OK) {
        m_lastError = failed(cs);
        emit lastErrorChanged();
        emit callChanged();
        return false;
    }
    sp_call_id call = 0;
    const sp_status st = sp_call_make(m_core, m_account, full,
                                      hs.isEmpty() ? nullptr : hs.constData(),
                                      (size_t)hs.size(), &call);
    if (st != SP_OK) {
        m_lastError = failed(st);
        emit lastErrorChanged();
        note(QStringLiteral("call failed: %1 (%2)")
                 .arg(QString::fromUtf8(sp_strerror(st)),
                      QString::fromUtf8(sp_last_error_detail(m_core))));
        emit callChanged();
        return false;
    }
    m_lastError.clear();
    emit lastErrorChanged();
    note(QStringLiteral("calling %1%2")
             .arg(QString::fromUtf8(full),
                  hs.isEmpty() ? QString()
                               : QStringLiteral(" with %1 extra header(s)")
                                     .arg(hs.size())));
    m_call = call;
    refreshCall();
    return true;
}

QString CoreBridge::exportTrace(const QString &callId) {
    if (!m_core || callId.isEmpty())
        return QString();
    const char *path = nullptr;
    if (sp_trace_export(m_core, callId.toUtf8().constData(), &path) != SP_OK)
        return QString();
    return QString::fromUtf8(path ? path : "");
}

QVariantMap CoreBridge::windowSize() const {
    QVariantMap m;
    int w = 0, h = 0;
    if (m_core && sp_ui_get_window_size(m_core, &w, &h) == SP_OK) {
        m["width"] = w;
        m["height"] = h;
    }
    return m;
}

void CoreBridge::saveWindowSize(int w, int h) {
    if (m_core)
        sp_ui_save_window_size(m_core, w, h);
}

QString CoreBridge::contactPicture(qlonglong id) const {
    if (!m_core || id == 0)
        return QString();
    const void *data = nullptr;
    size_t len = 0;
    const char *type = nullptr;
    if (sp_contact_picture(m_core, (int64_t)id, &data, &len, &type) != SP_OK
        || !data || len == 0)
        return QString();
    /* The core's type is a format name ("png"), not a media type, and
       a data: URI needs the media type -- "data:png;base64," is a URI
       no image loader accepts, so the avatar came back blank. PNG is
       the only format sp_contact_save stores. */
    (void)type;
    const QByteArray raw(static_cast<const char *>(data), (int)len);
    return QStringLiteral("data:image/png;base64,%1")
        .arg(QString::fromUtf8(raw.toBase64()));
}

QString CoreBridge::buildDate() const {
    return QString::fromUtf8(sp_build()->configured_date);
}

void CoreBridge::copyToClipboard(const QString &text) const {
    QGuiApplication::clipboard()->setText(text);
}

/* Android's audio in call mode while a call exists, and back after.
   pjmedia's Android driver records from VOICE_COMMUNICATION and plays
   on the voice-call stream, but switching the mode is the
   application's job: without it the platform's call routing, echo
   cancellation and in-call volume do not apply. Nothing on the
   desktop. */
void CoreBridge::syncAudioMode() {
#ifdef Q_OS_ANDROID
    const bool want = inCall();
    if (want == m_callAudioMode)
        return;
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return;
    QJniObject audio = ctx.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        QJniObject::fromString(QStringLiteral("audio")).object<jstring>());
    if (!audio.isValid())
        return;
    /* AudioManager.MODE_IN_COMMUNICATION and MODE_NORMAL. */
    audio.callMethod<void>("setMode", "(I)V", want ? 3 : 0);
    m_callAudioMode = want;

    /* The screen off while the handset is at an ear, which is the
       platform's own mechanism for it: a proximity wake lock, held for
       the call. Without it a cheek on the glass ended calls. Released
       waiting for the ear to move away, so a call that ends against
       the cheek does not light the screen there. */
    if (want && !m_proximity.isValid()) {
        QJniObject power = ctx.callObjectMethod(
            "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
            QJniObject::fromString(QStringLiteral("power")).object<jstring>());
        /* PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK */
        const jint level = 32;
        if (power.isValid()
            && power.callMethod<jboolean>("isWakeLockLevelSupported",
                                          "(I)Z", level)) {
            m_proximity = power.callObjectMethod(
                "newWakeLock",
                "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;",
                level,
                QJniObject::fromString(QStringLiteral("comlink:call"))
                    .object<jstring>());
        } else {
            fprintf(stderr, "comlink: no proximity sensor wake lock on "
                            "this device\n");
        }
    }
    if (m_proximity.isValid()) {
        const bool held = m_proximity.callMethod<jboolean>("isHeld", "()Z");
        if (want && !held) {
            m_proximity.callMethod<void>("acquire", "()V");
        } else if (!want && held) {
            /* PowerManager.RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY */
            m_proximity.callMethod<void>("release", "(I)V", 1);
        }
    }
    fprintf(stderr, "comlink: android audio mode %s\n",
            want ? "in-communication" : "normal");
#endif
}

/* The service is started and stopped from here rather than from QML,
   because "the core is up" is the fact that decides it and the core
   is what this object holds. */
void CoreBridge::setReachable(bool on) {
#ifdef Q_OS_ANDROID
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    if (!ctx.isValid())
        return;
    QJniObject name = QJniObject::fromString(
        QStringLiteral("io.github.etienne_gn.comlink.KeepAliveService"));
    QJniObject pkg = ctx.callObjectMethod("getPackageName",
                                          "()Ljava/lang/String;");
    QJniObject intent("android/content/Intent");
    intent.callObjectMethod("setClassName",
                            "(Ljava/lang/String;Ljava/lang/String;)"
                            "Landroid/content/Intent;",
                            pkg.object<jstring>(), name.object<jstring>());
    if (on) {
        /* startForegroundService, not startService: from Oreo on, a
           background start of an ordinary service is refused, and the
           service then has five seconds to call startForeground. */
        ctx.callObjectMethod("startForegroundService",
                             "(Landroid/content/Intent;)"
                             "Landroid/content/ComponentName;",
                             intent.object());
    } else {
        ctx.callMethod<jboolean>("stopService",
                                 "(Landroid/content/Intent;)Z",
                                 intent.object());
    }
#else
    Q_UNUSED(on);
#endif
}
