// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

struct PrefixBuildRequest {
    QString python;
    QString script;
    QString source;
    QString workspace;
    bool includePrs = false;
    int jobs = 1;
    QStringList cmakeArguments;
};
namespace LauncherPrefix {
bool selectInputs(const QJsonObject &discovery, bool includePrs, QJsonObject *selected, QString *error);
bool selectNestedInputs(const QJsonObject &discovery, bool includePrs, QJsonObject *selected, QString *error);
bool validateRequest(const PrefixBuildRequest &request, QString *error);
// Path of the heavy-build lock file in a private per-user directory, or empty with error set.
// DARLING_LAUNCHER_LOCK_DIR replaces $XDG_RUNTIME_DIR/darling-launcher for every user of the lock.
QString heavyBuildLock(QString *error);
}
class PrefixBuilder : public QObject {
    Q_OBJECT
public:
    explicit PrefixBuilder(QObject *parent = nullptr);
    ~PrefixBuilder() override;
    bool isRunning() const;
    void start(const PrefixBuildRequest &request);
signals:
    void output(const QString &text);
    void phaseChanged(const QString &phase);
    void completed(bool success, const QString &message, const QString &prefix, const QString &launcher, const QString &runtime);
private:
    enum class Phase { Idle, Inspect, Resolve, Checkout, ResolveNested, CheckoutNested, Build };
    QProcess process;
    QTemporaryDir staging;
    PrefixBuildRequest request;
    Phase phase = Phase::Idle;
    QJsonObject provenance;
    QString scriptSnapshot;
    QString phaseOutput;
    bool supportsNested = false;
    void run(Phase phase, const QStringList &arguments);
    void advance();
    void build();
    void fail(const QString &message);
};
