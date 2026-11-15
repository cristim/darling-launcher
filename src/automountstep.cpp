// SPDX-License-Identifier: GPL-3.0-or-later
#include "automountstep.h"
#include "discovery.h"
#include "mounthelpers.h"
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

AutoMount::StepResult HelperMountStep::run(const AutoMount::StepRequest &request) {
    AutoMount::StepResult result; result.exitCode = 1;
    QString error;
    AutoMount::Tools tools;
    tools.python = LauncherMount::trustedInterpreter(MountHelpers::helperPython(), MountHelpers::helperOwner(), &error);
    if (!tools.python.isEmpty()) {
        tools.mountScript = LauncherMount::trustedExecutable(QStringLiteral(LAUNCHER_MOUNT_SCRIPT), MountHelpers::helperOwner(), &error);
        if (request.kind == AutoMount::Kind::CacheVolume && LauncherMount::trustedCryptexScript(QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT), MountHelpers::helperOwner(), &error))
            tools.cryptexScript = QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT);
    }
    tools.apfsFuse = QSettings("cristim", "darling-launcher").value("apfsFuse", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfs-fuse")).toString();
    tools.mountRoot = LauncherMount::resolveHomePrefix(LauncherDiscovery::dataRoot() + "/mounts", LauncherMount::passwdHome());
    MountCommand command;
    if (tools.python.isEmpty() || tools.mountScript.isEmpty() || (request.kind == AutoMount::Kind::CacheVolume && tools.cryptexScript.isEmpty())
        || !AutoMount::commandFor(request, tools, &command, &error)) {
        result.output = error.isEmpty() ? "The mount helper is not installed root-owned (sudo cmake --install <build directory>)." : error;
        return result;
    }
    QProcess process; process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(command.program, command.arguments);
    if (!process.waitForStarted()) { result.output = process.errorString(); return result; }
    process.waitForFinished(-1); // the user may take a while in the authentication dialog
    result.output = QString::fromLocal8Bit(process.readAll());
    result.crashed = process.exitStatus() != QProcess::NormalExit;
    result.exitCode = process.exitCode();
    return result;
}
