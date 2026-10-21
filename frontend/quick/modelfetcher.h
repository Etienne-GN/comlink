#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QNetworkReply;

/* Fetches the speech model the transcript module needs, once, on the
 * operator's say-so, from a commit-pinned URL, and keeps it only if
 * its SHA-256 is the one written here. A file that does not match is
 * thrown away: a model is code the phone runs, and nobody checks it
 * after this. */
class ModelFetcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /* "idle", "downloading" or "error". */
    Q_PROPERTY(QString state READ state NOTIFY changed)
    /* Its own signal: progress ticks many times a second, and a
       listener reloading the model list on each one stats the disk
       for nothing. */
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit ModelFetcher(QObject *parent = nullptr);

    QString state() const { return m_state; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }

    /* [{name, sizeMb, present, path}], the recommended one first. */
    Q_INVOKABLE QVariantList models() const;
    Q_INVOKABLE void download(const QString &name);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool remove(const QString &name);

    /* Tests point these at local files. */
    QUrl baseUrl() const { return m_base; }
    void setBaseUrl(const QUrl &base) { m_base = base; }
    void setDir(const QString &dir) { m_dir = dir; }
    void setExpected(const QString &name, qint64 size, const QString &sha256);
    void setTransferTimeout(int ms) { m_timeoutMs = ms; }

signals:
    void changed();
    void progressChanged();
    void finished(const QString &name, const QString &path);

private:
    struct Model {
        QString name;
        QString file;
        qint64  size;
        QString sha256;
    };
    const Model *find(const QString &name) const;
    QString pathOf(const Model &m) const;
    void fail(const QString &why);
    void sweepParts() const;
    void take(QNetworkReply *reply);
    void onData();
    void onDone();

    QList<Model>           m_models;
    QUrl                   m_base;
    QString                m_dir;
    QNetworkAccessManager  m_net;
    QPointer<QNetworkReply> m_reply;
    QFile                  m_part;
    QCryptographicHash     m_hash { QCryptographicHash::Sha256 };
    QString                m_current;
    bool                   m_cancelled = false;
    bool                   m_writeFailed = false;
    QString                m_state = QStringLiteral("idle");
    qreal                  m_progress = 0;
    QString                m_error;
    int                    m_timeoutMs = 60000;
};
