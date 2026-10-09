# Launcher missing-symbol trace option (local branch only, not pushed)

Branch feat/missing-symbol-trace-option from origin/main; 3 commits (git log). Patch: fix.diff (git diff origin/main). Names provisional in src/tracevars.h. C++/Qt only, no helpers.

## Behaviour (v1, names-only log mode)
- Checkbox "Trace missing symbols" in the launch recovery dialog, remembered (recovery/trace). No mode combo, no raw words anywhere.
- Env (DARLING_MISSING_SYMBOLS=log, DARLING_MISSING_SYMBOLS_LOG=<path>) set only on the launched app's QProcess, only when checked. Off: any inherited DARLING_MISSING_SYMBOLS* is stripped.
- Elevated darling (setuid/setgid binary, unstatable binary, or euid 0): checkbox disabled with notice, variables never set.
- Log: <prefix>/.darling-launcher/trace/<app>-<UTC>.jsonl, dir 0700 user-owned, file 0600 created O_EXCL|O_NOFOLLOW via openat on the verified dir fd.
- Viewer ("Missing-symbol trace log..."): image, symbol, count, first-call order; "N lines skipped", read-cap notice; Open trace log folder (QDesktopServices); Export summary writes a new 0600 TSV (image, symbol, count) in the trace dir.
- Security fixes: saturating counters and caps (line 64 KiB, 10000 rows, 256 chars/text), control/format chars replaced in table and export, parse() opens O_NOFOLLOW + fstat regular file.

## Verified (heavy lock, flock -o, -j1, offscreen)
- launcher-trace-tests 14/14: off identical env, inherited vars stripped, on sets both vars under private dir, symlinked/exposed/foreign-owned dir refused, setuid/setgid fake binaries and euid 0 get no vars, garbage tolerated, counter saturation, row cap, hostile text cannot forge export lines, symlinked log not followed, summary 0600.
- GUI test (checkbox persistence, fake-darling launch off/on, elevated checkbox disabled) passes.

## NOT verified
- New regression tests were not run against the pre-fix code to see them fail (they were written with the fixes).
- Full launcher-gui-tests suite previously hit its 240 s ctest timeout on the loaded machine; not rerun in full after these changes.
- No real runtime consumer; JSONL schema (image, symbol, count) is a guess. Pre-commit Opus review passes not done.

## Open questions
- Platform: final env names/schema; does a setuid mldr ignore the vars (launcher currently refuses to set them when elevated).
- Compliance: checkbox label text is a placeholder.
- Security: parent .darling-launcher only checked for symlink/owner, not mode; stat failure of the darling path counts as elevated (fail closed).

## Gate evidence (added)
### Regression tests fail on pre-fix equivalents
Scratch copy of the tip with the fixes mutated out (no text cap/sanitizing, no saturation, no O_NOFOLLOW in parse, no env stripping, elevated ignored, greying removed). Output:
```
FAIL!  : TraceTest::elevatedDarlingNeverGetsVariables() 'env.isEmpty()' returned FALSE. ()
   Loc: [scratch/tests/trace_test.cpp(63)]
FAIL!  : TraceTest::offStripsInheritedVariables() Compared lists have different sizes.
   Actual   (env.keys()) size: 4
   Expected (QStringList{"KEEP"}) size: 1
   Loc: [scratch/tests/trace_test.cpp(73)]
FAIL!  : TraceTest::countersSaturateAndLongLinesAreSkipped() Compared values are not the same
   Actual   (summary.skipped): 0
   Expected (1)              : 1
   Loc: [scratch/tests/trace_test.cpp(82)]
FAIL!  : TraceTest::hostileTextCannotForgeOutput() 'c.isPrint() && c != '\t' && c != '\n' && c != QChar(0x202e)' returned FALSE. ()
   Loc: [scratch/tests/trace_test.cpp(95)]
FAIL!  : TraceTest::symlinkedLogIsNotFollowedAndSummaryIsPrivate() 'LauncherTrace::parse(other.path() + "/link.jsonl").unreadable' returned FALSE. ()
   Loc: [scratch/tests/trace_test.cpp(103)]
Totals: 9 passed, 5 failed, 0 skipped, 0 blacklisted, 90ms
FAIL!  : GuiTest::traceOptionPersistsAndOnlyTracedLaunchGetsVariables() '!box->isEnabled()' returned FALSE. ()
   Loc: [scratch/tests/gui_test.cpp(102)]
Totals: 2 passed, 1 failed, 0 skipped, 0 blacklisted, 110ms
```
Note: countersSaturate fails first on the long-line skip, which masks the saturation assert; rowCountIsCapped and parserCapsBytesRead pass on the mutation (row cap was not mutated).
### Full launcher-gui-tests (900 s cap via timeout, offscreen, flock -o)
- Base (origin/main): 39 passed, 1 failed (closeDuringLaunch: fatal error).
- Tip: 40 passed, 1 failed (same closeDuringLaunch). The extra pass is the new trace test; no other differences.
