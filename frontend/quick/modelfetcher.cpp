#include "modelfetcher.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

/* whisper.cpp's own model repository, pinned to one commit so the
   hashes below keep meaning something. */
static const char *const MODEL_BASE =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
    "5359861c739e955e79d9a303bcbc70fb988958b1/";

ModelFetcher::ModelFetcher(QObject *parent)
    : QObject(parent),
      m_base(QUrl(QString::fromLatin1(MODEL_BASE))),
      m_dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/models")) {
    /* small first: noticeably better at French than base, which is
       what an operator here speaks. */
    m_models = {
        { QStringLiteral("small"), QStringLiteral("ggml-small.bin"),
          487601967,
          QStringLiteral("1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b") },
        { QStringLiteral("base"), QStringLiteral("ggml-base.bin"),
          147951465,
          QStringLiteral("60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe") },
    };
}

void ModelFetcher::setExpected(const QString &name, qint64 size,
                               const QString &sha256) {
    for (Model &m : m_models)
        if (m.name == name) {
            m.size = size;
            m.sha256 = sha256;
        }
}

const ModelFetcher::Model *ModelFetcher::find(const QString &name) const {
    for (const Model &m : m_models)
        if (m.name == name)
            return &m;
    return nullptr;
}

QString ModelFetcher::pathOf(const Model &m) const {
    return m_dir + QLatin1Char('/') + m.file;
}

QVariantList ModelFetcher::models() const {
    QVariantList out;
    for (const Model &m : m_models) {
        QVariantMap v;
        v["name"] = m.name;
        v["sizeMb"] = (int)(m.size / (1024 * 1024));
        v["path"] = pathOf(m);
        v["present"] = QFileInfo(pathOf(m)).size() == m.size;
        out.append(v);
    }
    return out;
}

void ModelFetcher::fail(const QString &why) {
    m_part.close();
    /* m_part names a .part file only while a download owns one: after
       a rename it named the model itself, and removing that here
       deleted a good model on the next failure. */
    if (!m_part.fileName().isEmpty())
        m_part.remove();
    m_part.setFileName(QString());
    m_error = why;
    m_state = QStringLiteral("error");
    m_progress = 0;
    emit changed();
    emit progressChanged();
}

/* A download cut short by a crash or a killed app leaves its .part
   behind, and nothing else would ever remove it. Only one download
   runs at a time, so any .part present when one starts is stale. */
void ModelFetcher::sweepParts() const {
    QDir dir(m_dir);
    const QStringList parts =
        dir.entryList({ QStringLiteral("*.part") }, QDir::Files);
    for (const QString &p : parts)
        dir.remove(p);
}

void ModelFetcher::download(const QString &name) {
    const Model *m = find(name);
    if (!m || m_state == QStringLiteral("downloading"))
        return;
    if (!QDir().mkpath(m_dir)) {
        fail(tr("the models directory could not be created"));
        return;
    }
    sweepParts();
    m_part.setFileName(pathOf(*m) + QStringLiteral(".part"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("the model could not be written: %1").arg(m_part.errorString()));
        return;
    }
    m_hash.reset();
    m_current = name;
    m_cancelled = false;
    m_writeFailed = false;
    m_error.clear();
    m_progress = 0;
    m_state = QStringLiteral("downloading");
    emit changed();
    emit progressChanged();

    QNetworkRequest req(m_base.resolved(QUrl(m->file)));
    /* Inactivity, not total time: a slow link finishes, a dead one
       stops holding the page in "Downloading" forever. */
    req.setTransferTimeout(m_timeoutMs);
    /* Hugging Face answers with a redirect to its CDN. Followed, but
       never from https down to http. */
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_net.get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelFetcher::onData);
    connect(m_reply, &QNetworkReply::finished, this, &ModelFetcher::onDone);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64) {
                const Model *cur = find(m_current);
                if (cur && cur->size > 0) {
                    m_progress = qBound(0.0, (qreal)got / cur->size, 1.0);
                    emit progressChanged();
                }
            });
}

void ModelFetcher::take(QNetworkReply *reply) {
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty() || m_writeFailed)
        return;
    m_hash.addData(chunk);
    if (m_part.write(chunk) != chunk.size()) {
        m_writeFailed = true;
        reply->abort();
    }
}

void ModelFetcher::onData() {
    if (m_reply)
        take(m_reply);
}

void ModelFetcher::onDone() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply)
        return;
    reply->deleteLater();
    take(reply);

    if (m_cancelled) {
        m_part.close();
        m_part.remove();
        m_part.setFileName(QString());
        m_state = QStringLiteral("idle");
        m_progress = 0;
        emit changed();
        emit progressChanged();
        return;
    }
    if (m_writeFailed) {
        fail(tr("the model could not be written: %1").arg(m_part.errorString()));
        return;
    }
    /* The transfer timeout is TimeoutError on Qt 6.11 and a cancel on
       Debian 13's 6.8. An operator's cancel returned above, so a
       cancel here can only be the timeout. */
    if (reply->error() == QNetworkReply::TimeoutError
        || reply->error() == QNetworkReply::OperationCanceledError) {
        fail(tr("the download stalled: nothing arrived for %1 s")
                 .arg(m_timeoutMs / 1000));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("the download failed: %1").arg(reply->errorString()));
        return;
    }
    if (!m_part.flush()) {
        fail(tr("the model could not be written: %1").arg(m_part.errorString()));
        return;
    }
    m_part.close();

    const Model *m = find(m_current);
    const QString sha = QString::fromLatin1(m_hash.result().toHex());
    if (!m || m_part.size() != m->size || sha != m->sha256) {
        fail(tr("the download failed its checksum; nothing was kept"));
        return;
    }
    const QString final = pathOf(*m);
    QFile::remove(final);
    if (!m_part.rename(final)) {
        fail(tr("the model could not be put in place: %1")
                 .arg(m_part.errorString()));
        return;
    }
    m_part.setFileName(QString());
    m_state = QStringLiteral("idle");
    m_progress = 1;
    emit changed();
    emit progressChanged();
    emit finished(m->name, final);
}

void ModelFetcher::cancel() {
    if (!m_reply)
        return;
    m_cancelled = true;
    m_reply->abort();
}

bool ModelFetcher::remove(const QString &name) {
    const Model *m = find(name);
    if (!m || (m_state == QStringLiteral("downloading") && m_current == name))
        return false;
    const bool ok = !QFile::exists(pathOf(*m)) || QFile::remove(pathOf(*m));
    emit changed();
    return ok;
}
