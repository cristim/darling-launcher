# Launcher missing-symbol trace option (local branch only, not pushed)

Branch feat/missing-symbol-trace-option from origin/main, 1 commit (see `git log`). Names provisional in src/tracevars.h.

## Done
- src/tracevars.h constants; src/trace.{h,cpp} (env apply, 0700 dir/0600 file, symlink/foreign-owner refusal, tolerant JSONL parser, names-and-counts export); src/tracedialog.{h,cpp} (viewer: file picker, table, N lines skipped, size-cap notice, Open trace log folder via QDesktopServices/xdg-open, Export summary without raw words).
- Checkbox + mode combo (log default; Advanced text for trap/continue) in the launch recovery dialog, persisted as recovery/trace, recovery/traceMode. Env set only on the launched app's QProcess (Window::runCommand, "exec" launches).
- tests/fake-darling (fake consumer), tests/trace_test.cpp, GUI test traceOptionPersists...

## Verified (heavy lock, -j1, offscreen)
- launcher-trace-tests: 8/8 pass (off = identical env, on = both vars + path under private dir, symlink and exposed dir refused, /usr foreign-owned refused, garbage/huge tolerated).
- GuiTest: traceOptionPersistsAndOnlyTracedLaunchGetsVariables, rememberedImport..., workspacePreferences... pass.

## NOT verified
- Full launcher-gui-tests suite hit its 240 s ctest timeout on the loaded machine (the new test had passed within it); not rerun in full.
- No real runtime consumer; JSONL schema (image, symbol, count, regs) is my guess.
- Not reviewed by the two Opus passes; no README/AGENTS mention yet.

## Open questions
- Platform: final env names, JSONL schema, does a setuid mldr honour the vars, who creates the log (launcher pre-creates the 0600 file, O_EXCL).
- Compliance: label text; whether the viewer details column (raw words as hex) is allowed at all (export omits it); inherited DARLING_MISSING_SYMBOLS in the user shell is left untouched when the box is off (env byte-identical), so it still reaches the app.
- Security: trace dir under the prefix (.darling-launcher/trace); parent dir only checked for symlink/owner, not mode.
