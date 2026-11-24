#ifndef COMLINK_BACKUP_H
#define COMLINK_BACKUP_H

#include <QByteArray>
#include <QString>

/* A backup file holds every SIP password the phone has, so it can be
 * sealed with a passphrase: AES-256-GCM under a key PBKDF2 derives from
 * it. The sealed form starts with a line of text naming the format, so
 * a file can be told from a plain one without trying to open it. */
namespace Backup {

bool sealed(const QByteArray &file);

/* Empty on failure, with the reason in *why. */
QByteArray seal(const QByteArray &plain, const QString &passphrase,
                QString *why);

/* Empty on failure. A wrong passphrase and a damaged file are one
   answer: the authentication tag fails either way and nothing says
   which. */
QByteArray unseal(const QByteArray &file, const QString &passphrase,
                  QString *why);

}  // namespace Backup

#endif /* COMLINK_BACKUP_H */
