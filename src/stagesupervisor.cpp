// SPDX-License-Identifier: GPL-3.0-or-later
#include "stagesupervisor.h"
#include <dirent.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>
namespace {
// Total stop time stays below the launcher's StopGraceSeconds (10 s): 3 s grace + at most 4 s of sweeping.
constexpr int GraceSeconds = 3, SweepRounds = 200;
volatile sig_atomic_t stopRequested = 0, graceOver = 0;
void onStop(int) { stopRequested = 1; }
void onAlarm(int) { graceOver = 1; }
std::vector<pid_t> children() {
    std::vector<pid_t> out; const pid_t self = getpid();
    DIR *proc = opendir("/proc");
    if (!proc) return out;
    while (dirent *e = readdir(proc)) {
        char *end = nullptr; const long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char path[64], buf[512]; snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        const size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
        // "pid (comm) state ppid ...": comm may contain spaces and parentheses, so parse after the last ')'.
        const char *close = strrchr(buf, ')'); char state = 0; long ppid = 0;
        if (close && sscanf(close + 1, " %c %ld", &state, &ppid) == 2 && ppid == self) out.push_back(pid_t(pid));
    }
    closedir(proc);
    return out;
}
pid_t agent = -1;
int agentStatus = 0;
bool agentReaped = false;
// Reaps every exited child, remembering the agent's status (its pid must never be signalled once reaped).
void reap() {
    int st = 0; pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) if (p == agent) { agentStatus = st; agentReaped = true; }
}
// SIGKILLs every remaining descendant until none is left; returns how many, the agent not counted.
int sweep(int rounds) {
    std::set<pid_t> found;
    for (int i = 0; i < rounds; ++i) {
        reap();
        const auto kids = children();
        if (kids.empty()) break;
        for (pid_t k : kids) { kill(k, SIGKILL); if (k != agent) found.insert(k); }
        usleep(20000);
    }
    reap();
    return int(found.size());
}
}
int StageSupervisor::run(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s %s <agent> [args...]\n", argv[0], Flag); return 2; }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) { perror("stage-supervisor: PR_SET_CHILD_SUBREAPER"); return 2; }
    struct sigaction sa {}; sa.sa_handler = onStop; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); sigaction(SIGHUP, &sa, nullptr);
    sa.sa_handler = onAlarm; sigaction(SIGALRM, &sa, nullptr);
    agent = fork();
    if (agent < 0) { perror("stage-supervisor: fork"); return 2; }
    if (agent == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGALRM, SIG_DFL);
        execvp(argv[2], argv + 2);
        perror("stage-supervisor: exec"); _exit(127);
    }
    bool stopping = false;
    // Polled: a blocking wait could miss a stop signal that races it.
    while (!agentReaped && !graceOver) {
        reap();
        if (stopRequested && !stopping && !agentReaped) {
            stopping = true;
            kill(agent, SIGTERM);
            for (pid_t k : children()) if (k != agent) kill(k, SIGTERM);
            alarm(GraceSeconds);
        }
        if (!agentReaped) usleep(50000);
    }
    alarm(0);
    if (!agentReaped) kill(agent, SIGKILL); // not reaped yet, so the pid is still ours
    const int left = sweep(SweepRounds);
    if (left > 0) fprintf(stderr, "stage-supervisor: stopped %d process(es) the agent left running\n", left);
    if (!children().empty()) fprintf(stderr, "stage-supervisor: warning: processes the agent started are still running and escape supervision\n");
    if (!agentReaped) return 128 + SIGKILL;
    if (WIFEXITED(agentStatus)) return WEXITSTATUS(agentStatus);
    return 128 + (WIFSIGNALED(agentStatus) ? WTERMSIG(agentStatus) : 0);
}
