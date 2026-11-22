## Summary

Adds "Import framework resources" next to "Import language data". It copies the on-disk resource parts of macOS framework bundles whose code lives in the dyld shared cache, from the mounted macOS System volume into the same paths of the managed prefix.

Why: a cached framework that looks up its own bundle (for example CoreEmoji: `CoreEmoji framework bundle could not be found`, which stops Stocks in some runs) fails because the bundle folder does not exist on disk. The cache holds only code.

For every `System/Library/Frameworks/*.framework` and `System/Library/PrivateFrameworks/*.framework` (and framework bundles nested in `Versions/<v>/Frameworks`) it copies `Info.plist`, `version.plist` and the `Resources/` tree, keeping the `Versions/Current` layout and symlinks.

## Rules it enforces

- **No executable code.** Skipped: the framework binary, any file starting with a Mach-O or fat magic (checked by bytes, not names), `#!` scripts, compiled Metal libraries (`MTLB`) and compiled AppleScript (`FasdUAS`) by magic, `*.dylib`, `*.so`, `*.a`, `*.o`, `*.metallib`, `*.scpt` by name, and the directories `XPCServices`, `PlugIns`, `Helpers`, `MacOS`, `*.app`, `*.xpc`, `*.appex`, `*.kext` and nested `*.framework` inside `Resources`. Code is checked again when each file is copied, on the bytes actually written. Counts of what was skipped are shown, printed by the CLI and logged by the GUI.
- **Nothing the prefix already has is replaced.** A framework Darling ships keeps its files; only missing resource files are added. A differing file is reported as a conflict and left alone; an identical one counts as present. Symlinks in the prefix are never followed.
- **Size first.** The plan is built without writing anything (this is the dry run) and gives an on-disk estimate (4 KiB blocks per file, directory and symlink). The copy refuses when the estimate plus a 512 MiB reserve exceeds the free space, or the free space cannot be read. The GUI shows the total and asks before copying; the CLI needs `--yes`.
- **Idempotent and resumable.** Files are written under a fixed-length `.dlpart-<hash>` name, flushed, and renamed without replacing.
- **Reflink where possible** (`FICLONE`), then `copy_file_range`, then read/write. Hardlinks are not used: they would alias files on the user's Mac volume.

## Use

```
darling-launcher frameworks-import --volume DIR --prefix DIR --dry-run   # plan and size, copies nothing
darling-launcher frameworks-import --volume DIR --prefix DIR --yes       # copy
```

Without `--dry-run` or `--yes` it prints the plan, copies nothing and exits 2. GUI: select the volume and prefix, click "Import framework resources".

## Commits

1. `feat(safefs)`: `ensureDirectory`, `symlinkAt`, `MachO::hasMachOMagic`.
2. `feat(core)`: plan (dry run and estimate).
3. `feat(core)`: apply.
4. `test(core)`: synthetic volume tests.
5. `feat(cli)`: `frameworks-import`.
6. `feat(ui)`: the button, README.

## Verification

- New `launcher-frameworkresources-tests` (11 cases) on a synthetic volume (Mach-O, fat, metallib, compiled AppleScript and script stubs, XPC services, links, nested and flat frameworks, a Versions/Current layout), plus new cases in the safefs and macho tests. Probes confirmed that removing the Mach-O check, the code-directory skip and the copy-time re-check each fail a test.
- `frameworks-import --dry-run` run with the built binary against a fixture volume: nothing written to the prefix.
- A cold adversarial review (two passes, same reviewer) found and led to fixes for long file names, deletion of foreign `.dlpart` files, an estimate that ignored disk blocks, unknown free space, and code swapped in between plan and copy.

**Not run against a real prefix or a real mounted macOS System volume yet.** Only fixtures were used.

## Known limits

- The GUI copy has no cancel or progress bar, and the window will not close while it runs. A rerun resumes.
- A tree nested deeper than 32 levels fails the whole import.
- No MCP tool for this import yet.

## Pre-existing test failures on main

Both fail the same way without this branch (full ctest: 32 of 34 pass):

- `launcher-gui-tests`: times out (an origin/main build also exceeded 260 s without finishing).
- `launcher-handoff-tests`: `HandoffCheckTest::vendoredOracleIsTheConfirmedVersion`; the pinned SHA-256 in `tests/handoff_check_test.cpp` does not match `tests/handoff-oracle/check_handoff.py`, which is identical to origin/main.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
