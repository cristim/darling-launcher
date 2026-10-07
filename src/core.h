#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct AppEntry {
    QString name;
    QString relativeBundle;
    QString executable;
    QString sourceRelative;
};

struct MissingSymbol {
    QString symbol;
    QString referencedFrom;
    QString expectedIn;
    bool valid() const { return !symbol.isEmpty() && expectedIn.startsWith('/'); }
};

namespace LauncherCore {
QStringList discoverApps(const QString &volume);
QStringList mountedMacVolumes();
bool looksLikeMacVolume(const QString &root);
bool validateLocations(const QString &volume, const QString &prefix, QString *error);
bool importApp(const QString &volume, const QString &prefix, const QString &relativeBundle, AppEntry *result, QString *error);
bool importLibrary(const QString &volume, const QString &prefix, const QString &expectedIn, QString *error);
MissingSymbol diagnose(const QString &output);
QString catalogPath(const QString &prefix);
QJsonObject loadCatalog(const QString &prefix);
bool saveCatalog(const QString &prefix, const QJsonObject &catalog, QString *error);
bool stageBrewfile(const QString &prefix, const QString &source, QString *guestPath, QString *error);
QString issueDraft(const AppEntry &app, const QJsonArray &chain, const QString &output,
                   const QString &volume, const QString &prefix, const QString &darling);
}
