// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace TraceVars {
inline constexpr char Mode[] = "DARLING_MISSING_SYMBOLS";
inline constexpr char Log[] = "DARLING_MISSING_SYMBOLS_LOG";
inline constexpr char ModeLog[] = "log";
inline constexpr char DataDirName[] = ".darling-launcher";
inline constexpr char TraceDirName[] = "trace";
// DARLING_MISSING_SYMBOLS_LOG is a directory opened by dyld from the guest root.
// The prefix root is the guest root, so the launcher's trace directory is this
// absolute guest path; dyld rejects a file or a host path that the guest cannot see.
inline constexpr char TraceDirGuestPath[] = "/.darling-launcher/trace";
}
