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

Choose a mounted macOS volume and a separate prefix. **Detect mounted macOS volumes** lists current mounts with a macOS directory layout. **Create prefix** opens the isolated VibeDarling build workflow described below. **Initialize selected prefix** initializes a directory with the already configured Darling runtime. **Scan volume** automatically lists `.app` bundles in subdirectories of `Applications` and `System/Applications`; select any set to copy into the prefix. Imports reject source escapes, unsafe symlinks, and existing targets. Bundle metadata is read with libplist; executable bytes are copied privately and never inspected.

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

App scanning includes subdirectories of `Applications` and `System/Applications`, including Utilities, and requires a usable `CFBundleExecutable` in `Info.plist`. It skips directory symlinks and does not scan inside app bundles. It does not configure native Homebrew or resolve failures for which the loader supplies no absolute `Expected in` path. Framework imports copy a whole framework bundle. A successful Darling exit proves the launcher invocation completed, not that the guest app's UI worked. Real import and launch verification needs a user-selected mounted macOS source and an appropriate Darling runtime.

### Guided path setup

On startup, a single usable mounted macOS source is selected automatically.
Multiple sources require a choice; no partition is mounted automatically.
Settings lists existing mounts even when they contain only recovery or firmware
data, with readability and app/library suitability shown separately. Discovery
reads the live mount table, including generic APFS FUSE mounts, and supports its
`root` wrapper directory. The list refreshes automatically and through **Detect
mounted macOS volumes**. In the mount dialog, selecting a partition lists all of
its existing volumes: **Use existing source** reuses a suitable mount without
authorization or another mount operation. Multiple usable volumes require an
explicit choice; mounted partitions cannot be mounted again through the individual
mount action.
**Detect paths** also finds an installed `darling` executable and reads runtime
paths from the selected prefix's launcher build provenance.

In **Create prefix**, **Find local tools and sources** searches immediate Darling
folders in `~/src`, the launcher's data sources folder, and `/tmp`. Multiple
matches are presented for selection. Paths remain editable. A fresh workspace
under the launcher's application data folder is proposed visibly.
**Clone VibeDarling…** clones `https://github.com/VibeDarling/darling.git` into a
new folder under a parent you select, streams Git progress, and fills the source
field. Existing checkouts are not modified. Submodule integration remains the
prefix builder's responsibility. The prefix-builder script must be present in
that clone or selected from the separate tooling checkout; the launcher does
not assume the tooling branch has been merged upstream. Clone failures retain
partial folders for inspection, and require an explicit retry.

### Apps, settings and contributions

The **Apps** tab contains the import browser, imported apps, launches and progress.
Configure source, prefix and runtime paths in **Settings**; **Apply settings**
returns to Apps and reloads the selected source and prefix.

A contribution offer appears only for a selected app whose missing-symbol launch
failure was followed by a successful library import into the private prefix.
It explains the imported libraries and retry result, and offers a local issue
draft or the separate opt-in source-fix workflow. The offer also remains available
if the retry still fails. Failed imports, ordinary failures and launches without
this workaround show no contribution actions. Original loader errors are retained
in each private dependency-chain step and included in the proposed draft. Review
local paths and logs before sharing; Apple payloads remain private and no issue or
PR is submitted automatically. Source-fix opt-in currently presents workflow
guidance; automated source edits and PR creation remain future work.

### Machine defaults and a private source build

With no saved prefix, the launcher proposes its own `prefixes/default` location
under Qt's application data directory. It does not select another tab's test
prefix. A verified installed launcher/runtime pair is populated automatically;
**Detected runtimes** offers other complete discovered build/image pairs. Saved
paths remain editable, and a prefix's build provenance takes priority when
recovering its runtime paths.

**Build and deploy Darling…** defaults the builder script, a clean VibeDarling
main/master source clone (or a visible proposed clone path), and a fresh workspace.
Building from a missing source path clones upstream first, then invokes the
separate builder to integrate root and nested repositories, build, deploy a
private image and initialize its prefix. Choose default branches or include open
PRs. Successful completion selects all three resulting paths automatically.
This does not install over the system runtime or change other checkouts.

### Mount every detected macOS volume

**Settings → Mount macOS source… → Mount all detected volumes read-only** scans
APFS/HFS partitions detected by `lsblk`. For APFS it uses `apfsutil` metadata to
obtain actual volume indices, then mounts each unencrypted volume with APFS FUSE.
It does not guess indices or unlock FileVault. The complete batch uses one `pkexec` authorization request and
`ro,nodev,nosuid,noexec`; cancelling or denying authorization stops the batch.
Mount helpers and the batch mount root are configurable. Installed helpers or
helpers built in discovered Darling folders are detected automatically.

After mounting, a single macOS applications source is selected automatically;
multiple matches require a choice. Verified mounts are listed in the dialog's
session mount selector for explicit unmounting. They remain mounted on exit.
A working desktop polkit authentication agent is required. The launcher does not
install kernel drivers or attempt an alternate privilege route after denial.

For an explicitly requested desktop batch mount, start the launcher with
`--mount-all`. It opens Settings and begins the batch after partition discovery.
Launch it from your desktop session so `pkexec` can reach that session's existing
polkit agent; setting display variables alone does not create a login-session
association. Ordinary startup never mounts partitions automatically.

### One authorization for the whole mount batch

The GUI uses `tools/mount-macos.py`, which requests authorization exactly once,
then enumerates and mounts every detected unencrypted APFS/HFS volume in that
privileged helper. Python runs in isolated mode. The helper accepts only detected
block devices, obtains APFS volume indices from filesystem metadata, requires
empty directories without symlink ancestors, and verifies read-only mount flags.
Progress and discovered application sources are emitted as JSON lines. Individual
driver failures are reported and the remaining detected volumes are attempted;
there is no authorization retry or alternate privilege mechanism.

You can run the same script from your desktop terminal:

```sh
python3 tools/mount-macos.py --mount-root "$XDG_RUNTIME_DIR/darling-launcher/macos" \
  --apfs-fuse /absolute/path/to/apfs-fuse --apfsutil /absolute/path/to/apfsutil
```

The script uses the calling user's identity supplied by `pkexec` for file access
ownership. It never writes filesystem data to the macOS device or unlocks encrypted
volumes. Mounts remain present until explicitly unmounted. Apple payloads and
filesystem metadata are private local data and are not uploaded.
