// SPDX-License-Identifier: GPL-3.0-or-later
// Preload shim for launcher-safefs-owner-tests: makes geteuid() report DYLDCACHE_FAKE_EUID, so the owner check of
// SafeFs::openWrite sees a file "owned by another user" without root.
#include <cstdlib>
#include <unistd.h>

extern "C" uid_t geteuid(void) {
    const char *fake = std::getenv("DYLDCACHE_FAKE_EUID");
    return fake ? uid_t(std::atoi(fake)) : 0;
}
