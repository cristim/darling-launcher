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
#include <vector>
namespace {
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
void reap() { while (waitpid(-1, nullptr, WNOHANG) > 0) {} }
// Signals every remaining descendant until none is left; returns how many were found.
int sweep(int signal, int rounds) {
    int found = 0;
    for (int i = 0; i < rounds; ++i) {
        reap();
        const auto kids = children();
        if (kids.empty()) break;
        found += int(kids.size());
        for (pid_t k : kids) kill(k, signal);
        usleep(20000);
    }
    reap();
    return found;
}
}
int StageSupervisor::run(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s %s <agent> [args...]\n", argv[0], Flag); return 2; }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) { perror("stage-supervisor: PR_SET_CHILD_SUBREAPER"); return 2; }
    struct sigaction sa {}; sa.sa_handler = onStop; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); sigaction(SIGHUP, &sa, nullptr);
    sa.sa_handler = onAlarm; sigaction(SIGALRM, &sa, nullptr);
    const pid_t agent = fork();
    if (agent < 0) { perror("stage-supervisor: fork"); return 2; }
    if (agent == 0) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGALRM, SIG_DFL);
        execvp(argv[2], argv + 2);
        perror("stage-supervisor: exec"); _exit(127);
    }
    int status = 0; bool stopping = false;
    for (;;) {
        int st = 0; const pid_t p = waitpid(-1, &st, WNOHANG); // polled: a blocking wait could miss a signal racing it
        if (p == agent) { status = st; break; }
        if (p < 0 && errno == ECHILD) break;
        if (p == 0) usleep(50000);
        if (stopRequested && !stopping) {
            stopping = true;
            kill(agent, SIGTERM);
            sweep(SIGTERM, 1);
            alarm(5);
        }
        if (graceOver) kill(agent, SIGKILL);
    }
    alarm(0);
    if (const int left = sweep(SIGKILL, 500)) fprintf(stderr, "stage-supervisor: stopped %d process(es) the agent left running\n", left);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
