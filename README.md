# Darling Launcher

A Linux host Qt launcher for user-owned Darling prefixes. It never runs inside a prefix. The mounted macOS source volume, prefix, and Darling executable are selected in the GUI and remembered locally. No Apple app, library, symbol dump, or prefix data belongs in this repository.

Licensed under the GNU General Public License, version 3 or any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE). The license applies to this launcher's source; imported Apple payloads remain private user data with their existing licenses.

## Build and run

Requires CMake, a C++17 compiler, Qt 6 Core/Widgets/Concurrent/Test, Qt image format plugins (ICNS), and `libplist-2.0`.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/darling-launcher
```

The core tests use synthetic app bundles, libraries, and Brewfiles. Mount tests cover partition discovery, fixed read-only arguments, and fake pkexec success/cancellation/denial. Prefix-builder tests cover both input scopes, lock preservation and integration failure. The offscreen GUI tests decode a synthetic ICNS icon, exercise keyboard and mouse selection in list/grid views, drag an app into the import area before a fake Darling diagnosis/import/retry sequence, and verify that a built prefix selects its own runtime image.

Choose a mounted macOS volume and a separate prefix. **Detect mounted macOS volumes** lists current mounts with a macOS directory layout. **Create prefix** opens the isolated VibeDarling build workflow described below. **Initialize selected prefix** initializes a directory with the already configured Darling runtime. **Scan volume** lists top-level `.app` bundles from `Applications` and `System/Applications`; select any set to copy into the prefix. Imports reject source escapes, unsafe symlinks, and existing targets. Bundle metadata is read with libplist; executable bytes are copied privately and never inspected.

**Mount macOS source** lists detected APFS/HFS partitions using `lsblk`. Choose a partition, an existing empty directory, a kernel driver or APFS FUSE, and an explicit APFS volume index. It invokes the selected driver through `pkexec` with read-only, nodev, nosuid and noexec options. The kernel path requires an installed filesystem module; APFS FUSE requires `apfs-fuse`. Authorization cancellation and denial stop the action. The source field is updated only after verifying the mount and macOS layout. You can explicitly unmount a source mounted during this launcher session; it is not unmounted on exit. Encrypted APFS unlocking is outside this workflow. Driver option references: [APFS FUSE](https://github.com/sgan81/apfs-fuse) and [Linux APFS](https://github.com/eafer/linux-apfs-rw).

The source browser reads bundle display names, `CFBundleIconFile` resources and total file sizes in the background. List view uses 32-pixel icons and shows sizes in decimal units; grid view uses 64-pixel icons. Ctrl+A, Ctrl-click and Shift-click select sets. Drag a set into the imported-apps area or use **Import selected apps**. Missing or unreadable icon resources use a generic icon; no executable code is read to obtain icons.

## Building a prefix from VibeDarling branches and PRs

Configure the external `all-vibedarling-pr-prefix.py` from the VibeDarling tooling tab, a clean independent clone at the current upstream default branch, a new workspace whose parent exists, and the job count. Choose default branches only or default branches plus all open PRs. The script resolves main/master where present and records exceptions for repositories with another default branch. Optional CMake settings are passed as one `-DNAME=VALUE` per line.

The launcher snapshots the selected script and checks its CLI. It runs `resolve`, `checkout` and `build`, including `resolve-nested` and `checkout-nested` when that interface is available. For branch-only builds it creates separate top-level and nested locks with empty PR lists; original discovery locks are preserved. Existing workspaces and workspaces inside the source checkout are refused. Conflicts and script failures stop the workflow and retain the workspace for inspection.

The output prefix is `<workspace>/prefix`, the executable is `<workspace>/build/src/startup/darling`, and the install root is `<workspace>/image/usr/local`. The GUI selects these together after successful verification and sets `DARLING_INSTALL_PREFIX` for guest launches. Script, lock and integration-manifest hashes are recorded under the private prefix's `.darling-launcher/build-provenance.json`. No existing source checkout or installed runtime is modified by this integration. Keep the GUI open while imports and builds are active.

You can preselect the external script when launching:

```sh
./build/darling-launcher --prefix-builder /path/to/all-vibedarling-pr-prefix.py
```

The external builder requires GitHub network access and a full Darling build environment. It currently resolves the PR inventory for both choices; only the selected lock controls which PRs are integrated.

**Launch selected** uses `DPREFIX=<selected prefix> <selected darling> exec <guest executable>`. Each launch has a separate host `QProcess`, and the progress/output area shows its activity and exit status. If dyld reports `Symbol not found`, `Referenced from`, and `Expected in`, the GUI offers to copy the named system library or framework from the selected mounted volume and retry. Each accepted import is added to a private chain at `<prefix>/.darling-launcher/catalog.json`. The proposed issue is saved locally under that directory for review. Nothing is submitted automatically.

**Install Brewfile** stages a copy under the prefix and runs only `/opt/homebrew/bin/brew` as a guest executable. Native guest Homebrew must already be installed there. Brewfiles with MAS entries are refused because Apple ID interaction needs a separate workflow. The Brewfile is never executed on Linux.

The **Opt in: source fix workflow** action describes a separate clean-room VibeDarling patch and PR process. It does not change another checkout or submit a PR. A real source fix should use published source, headers, documentation and API observations, with a completed patch and PR draft presented for explicit approval.

## Current limits

App scanning covers top-level stock apps and requires a usable `CFBundleExecutable` in `Info.plist`. It does not discover nested utilities, configure native Homebrew, or resolve failures for which the loader supplies no absolute `Expected in` path. Framework imports copy a whole framework bundle. A successful Darling exit proves the launcher invocation completed, not that the guest app's UI worked. Real import and launch verification needs a user-selected mounted macOS source and an appropriate Darling runtime.
