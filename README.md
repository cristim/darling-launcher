# Darling Launcher

A Linux host Qt launcher for user-owned Darling prefixes. It never runs inside a prefix. The mounted macOS source volume, prefix, and Darling executable are selected in the GUI and remembered locally. No Apple app, library, symbol dump, or prefix data belongs in this repository.

## Build and run

Requires CMake, a C++17 compiler, Qt 6 Core/Widgets/Concurrent/Test, and `libplist-2.0`.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/darling-launcher
```

Choose a mounted macOS volume and a separate prefix. **Detect mounted macOS volumes** lists currently mounted APFS/HFS volumes with a macOS directory layout; it never mounts a partition. **Create prefix** asks Darling to initialize the selected directory. **Scan volume** lists top-level `.app` bundles from `Applications` and `System/Applications`; select any set to copy into the prefix. Imports reject source escapes, unsafe symlinks, and existing targets. Bundle metadata is read with libplist; executable bytes are copied privately and never inspected.

**Launch selected** uses `DPREFIX=<selected prefix> <selected darling> exec <guest executable>`. Each launch has a separate host `QProcess`, and the progress/output area shows its activity and exit status. If dyld reports `Symbol not found`, `Referenced from`, and `Expected in`, the GUI offers to copy the named system library or framework from the selected mounted volume and retry. Each accepted import is added to a private chain at `<prefix>/.darling-launcher/catalog.json`. The proposed issue is saved locally under that directory for review. Nothing is submitted automatically.

**Install Brewfile** stages a copy under the prefix and runs only `/opt/homebrew/bin/brew` as a guest executable. Native guest Homebrew must already be installed there. Brewfiles with MAS entries are refused because Apple ID interaction needs a separate workflow. The Brewfile is never executed on Linux.

The **Opt in: source fix workflow** action describes a separate clean-room VibeDarling patch and PR process. It does not change another checkout or submit a PR. A real source fix should use published source, headers, documentation and API observations, with a completed patch and PR draft presented for explicit approval.

## Current limits

App scanning covers top-level stock apps and requires a usable `CFBundleExecutable` in `Info.plist`. It does not discover nested utilities, configure native Homebrew, or resolve failures for which the loader supplies no absolute `Expected in` path. Framework imports copy a whole framework bundle. A successful Darling exit proves the launcher invocation completed, not that the guest app's UI worked. Real import and launch verification needs a user-selected mounted macOS source and an appropriate Darling runtime.
