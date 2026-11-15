// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mount.h"
#include <QString>
#include <unistd.h>

// Who must own the root-run helpers and which interpreter runs them. Test builds (LAUNCHER_TEST_USER_OWNED_HELPERS) use
// user-owned fakes; production accepts only root-owned ones. Shared by the mount dialog and the explained mount step.
namespace MountHelpers {
// Test builds run user-owned fake helpers; production accepts only root-owned ones.
inline unsigned helperOwner() {
#ifdef LAUNCHER_TEST_USER_OWNED_HELPERS
    return getuid();
#else
    return 0;
#endif
}
// The one interpreter root runs: chosen at configure time (cmake/SelectPython.cmake); test builds point it at a fake.
inline QString helperPython() {
#ifdef LAUNCHER_TEST_USER_OWNED_HELPERS
    return QStringLiteral(LAUNCHER_PYTHON);
#else
    return LauncherMount::launcherPython();
#endif
}
}
