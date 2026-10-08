// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"
#include "discovery.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

namespace LauncherLog {
static constexpr qint64 RotateBytes = 8 * 1024 * 1024;
QString path() { return LauncherDiscovery::dataRoot() + "/logs/launcher.log"; }
void write(const QString &source, const QString &text) {
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    const QString file = path();
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return;
    if (QFileInfo(file).size() > RotateBytes)
        QFile::rename(file, file + "." + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss"));
    QFile log(file);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QString stamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (const QString &line : text.split('\n'))
        if (!line.trimmed().isEmpty()) log.write((stamp + " [" + source + "] " + line + "\n").toUtf8());
}
void installQtHandler() {
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        const char *level = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warning" : "error";
        write(QString("qt-") + level, message);
        fprintf(stderr, "%s\n", qPrintable(message));
    });
}
}
