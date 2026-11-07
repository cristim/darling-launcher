// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "libraries.h"
#include "macho.h"
#include <QByteArray>
#include <QMap>
#include <QString>

struct FetchedImage {
    QByteArray bytes;
    LibrarySource source; // source.sha256 is computed by the source; verifyFetched checks it
};

enum class FetchStatus { Ok, NotFound, Error };
struct ImageClosure : MachO::Closure {
    QStringList invalid; // dependency names that failed validInstallName; never passed to a source
};

// Where library images come from (cache extraction, a mounted volume, test fixtures).
class ImageSource {
public:
    virtual ~ImageSource() = default;
    // NotFound means the source does not have the image (fine); Error means it could not answer
    // (I/O error, refused gate, crash) and callers must fail closed.
    virtual FetchStatus fetch(const QString &installName, FetchedImage *out, QString *error) = 0;
    // Dependency closure of an app executable. Names are validated before any fetch(); a source Error
    // or a failed verifyFetched() fails the closure instead of reporting the image as missing.
    virtual bool closure(const QByteArray &appExecutable, quint32 preferredSubtype, bool allowFallback,
                         ImageClosure *out, QString *error);
};

namespace ImageSources {
QString sha256Hex(const QByteArray &bytes);
// Sources are untrusted and their own sha256 is not trusted: on success image->source.sha256 is
// overwritten with the launcher-computed hash, and import must record that one. Refuses empty bytes,
// a non-importable install name, or a source sha256 that disagrees with the delivered bytes.
bool verifyFetched(const QString &installName, FetchedImage *image, QString *error);
}

class FixtureSource : public ImageSource {
public:
    void add(const QString &installName, const QByteArray &bytes) { images_.insert(installName, bytes); }
    FetchStatus fetch(const QString &installName, FetchedImage *out, QString *error) override;
private:
    QMap<QString, QByteArray> images_;
};
