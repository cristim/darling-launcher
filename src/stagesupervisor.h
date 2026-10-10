// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// `<launcher> --stage-supervisor <agent> [args...]`: runs one pipeline stage's agent as a child
// subreaper, so processes it leaves behind (setsid, double fork) are reparented here and killed
// when the agent exits or the supervisor gets SIGTERM. Exits with the agent's status.
namespace StageSupervisor {
inline constexpr const char *Flag = "--stage-supervisor";
int run(int argc, char **argv);
}
