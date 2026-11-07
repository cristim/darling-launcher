// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagesource.h"
#include "core.h"
#include <QCryptographicHash>
#include <optional>

namespace ImageSources {
QString sha256Hex(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool verifyFetched(const QString &installName, FetchedImage *image, QString *error) {
    using LauncherCore::fail;
    if (!Libraries::validInstallName(installName, error)) return false;
    if (image->bytes.isEmpty()) return fail(error, "Source delivered an empty image for " + installName);
    const QString computed = sha256Hex(image->bytes);
    if (image->source.sha256.compare(computed, Qt::CaseInsensitive) != 0)
        return fail(error, "Source sha256 does not match the delivered bytes for " + installName);
    image->source.sha256 = computed;
    return true;
}
}

bool ImageSource::closure(const QByteArray &appExecutable, quint32 preferredSubtype, bool allowFallback,
                          ImageClosure *out, QString *error) {
    QString hardError; QStringList invalid;
    const auto resolver = [&](const QString &name) -> std::optional<QByteArray> {
        if (!hardError.isEmpty()) return std::nullopt;
        QString err;
        if (!Libraries::validInstallName(name, &err)) { invalid.append(name); return std::nullopt; }
        FetchedImage image;
        const FetchStatus status = fetch(name, &image, &err);
        if (status == FetchStatus::NotFound) return std::nullopt;
        if (status == FetchStatus::Error) { hardError = "Source failed for " + name + ": " + err; return std::nullopt; }
        if (!ImageSources::verifyFetched(name, &image, &err)) { hardError = err; return std::nullopt; }
        return image.bytes;
    };
    MachO::Closure c;
    if (!MachO::dependencyClosure(appExecutable, resolver, preferredSubtype, allowFallback, &c, error)) return false;
    if (!hardError.isEmpty()) return LauncherCore::fail(error, hardError);
    ImageClosure result;
    static_cast<MachO::Closure &>(result) = c;
    result.invalid = invalid;
    for (const QString &name : invalid) { result.installNames.removeAll(name); result.unresolved.removeAll(name); }
    *out = result;
    return true;
}

FetchStatus FixtureSource::fetch(const QString &installName, FetchedImage *out, QString *error) {
    const auto it = images_.constFind(installName);
    if (it == images_.cend()) { LauncherCore::fail(error, "Fixture has no image for " + installName); return FetchStatus::NotFound; }
    *out = {*it, {"fixture", "fixture", "fixture-1", ImageSources::sha256Hex(*it)}};
    return FetchStatus::Ok;
}
