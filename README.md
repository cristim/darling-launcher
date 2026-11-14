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

The core tests use synthetic app bundles, libraries, and Brewfiles. Mount tests cover partition discovery, fixed read-only arguments, and fake pkexec success/cancellation/denial. Prefix-builder tests cover default-branch-only inputs, lock preservation and integration failure. The offscreen GUI tests decode a synthetic ICNS icon, exercise keyboard and mouse selection in list/grid views, drag an app into the import area before a fake Darling diagnosis/import/retry sequence, and verify that a built prefix selects its own runtime image.

Choose a mounted macOS volume and a separate prefix. **Detect mounted macOS volumes** lists current mounts with a macOS directory layout. **Create prefix** opens the isolated VibeDarling build workflow described below. **Initialize selected prefix** initializes a directory with the already configured Darling runtime. **Scan volume** automatically lists `.app` bundles in subdirectories of `Applications` and `System/Applications`; select any set to copy into the prefix. Imports reject source escapes, unsafe symlinks, and existing targets. Bundle metadata is read with libplist; executable bytes are copied privately and never inspected.

**Import language data** copies `/usr/share/langid` (for example `langid.inv` and `langid_base.inv`) from the selected mounted macOS volume to the same path in the selected prefix. Frameworks served from the dyld shared cache, such as Calculator's text stack, assert that these files exist. The data is Apple's and comes only from your own Mac at run time; it is never committed or bundled. The source is read-only, symlinks that leave the directory are rejected, an identical existing copy is reported as already present, a different existing copy is left unchanged, and a source without the directory (older or newer macOS) reports that nothing was copied. The same import runs headless as `darling-launcher langid-import --volume DIR --prefix DIR`.

**Import framework resources** copies the on-disk resource parts of the system framework bundles whose code lives in the dyld shared cache. A cached framework that looks up its own bundle (for example CoreEmoji, whose "framework bundle could not be found" assertion can stop Stocks) fails because the bundle folder does not exist; the cache holds only code. For every `System/Library/Frameworks/*.framework` and `System/Library/PrivateFrameworks/*.framework` on the selected volume (and framework bundles nested in `Versions/<v>/Frameworks`) it copies `Info.plist`, `version.plist` and the `Resources/` tree to the same path in the prefix, keeping the `Versions/Current` layout and symlinks. The data is Apple's and comes only from your own Mac at run time.

- **No code is copied.** The framework binary, any file that starts with a Mach-O or fat magic (checked by bytes, not names), `#!` scripts, compiled Metal libraries (`MTLB`) and compiled AppleScript (`FasdUAS`) by magic, `*.dylib`, `*.so`, `*.a`, `*.o`, `*.metallib`, `*.scpt` by name, and the directories `XPCServices`, `PlugIns`, `Helpers`, `MacOS`, `*.app`, `*.xpc`, `*.appex`, `*.kext` and nested `*.framework` inside `Resources` are skipped, and the counts are shown and logged. Symlinks that are absolute, dangling or leave their framework are refused and listed; a link to code is dropped.
- **Nothing the prefix already has is replaced.** A framework Darling ships keeps its binary and its files; only missing resource files are added. An existing file that differs is listed as a conflict and left unchanged, an identical one is reported as present.
- **Size first, then confirm.** The button measures the volume without writing, then asks with the total (decimal units), the counts and the free space of the prefix. The total is an estimate of what the files take on disk (4 KiB blocks per file, directory and symlink); the import refuses when it plus a 512 MiB reserve does not fit, or when the free space cannot be read. Code is checked again at the moment each file is copied. Files are cloned (reflink) when the source and the prefix are on one filesystem that supports it, then copied in the kernel, then with read and write; hardlinks are not used because they would alias files on your Mac volume.
- **Resumable.** Each file is written under a `.dlpart-<hash>` name, flushed and renamed into place without replacing, so an interrupted run leaves no half file under a real name and running it again continues.

Headless: `darling-launcher frameworks-import --volume DIR --prefix DIR --dry-run` prints the plan and the size estimate and copies nothing; add `--yes` instead of `--dry-run` to copy. Without either, the plan is printed, nothing is copied and the exit code is 2. `--only NAME[,NAME...]` limits the import to the named framework bundles (base names such as `CoreEmoji`, matched in `Frameworks` and `PrivateFrameworks`; frameworks nested in a named one come along); every other rule is unchanged, and a name that matches no bundle is listed and fails the run with exit 1. The GUI button always imports every framework.

**Mount macOS source** lists detected APFS/HFS partitions using `lsblk`. Choose a partition, a kernel driver or APFS FUSE, and an APFS volume index (required for the kernel driver). It runs the same root-owned helper as the batch mount (`tools/mount-macos.py --device ... --volume ... [--kernel]`) through `pkexec`, which mounts under `/run/darling-launcher/<uid>/` with read-only, nodev, nosuid and noexec options. The kernel path requires an installed filesystem module; APFS FUSE requires `apfs-fuse`. Authorization cancellation and denial stop the action. The source field is updated only after verifying the mount and macOS layout. You can explicitly unmount a source the helper mounted during this launcher session (only mount points under that root-owned directory are accepted); it is not unmounted on exit. Encrypted APFS unlocking is outside this workflow. Driver option references: [APFS FUSE](https://github.com/sgan81/apfs-fuse) and [Linux APFS](https://github.com/eafer/linux-apfs-rw).

The source browser reads bundle display names, `CFBundleIconFile` resources and total file sizes in the background. List view uses 32-pixel icons and shows sizes in decimal units; grid view uses 64-pixel icons. Ctrl+A, Ctrl-click and Shift-click select sets. Drag a set into the imported-apps area or use **Import selected apps**. Missing or unreadable icon resources use a generic icon; no executable code is read to obtain icons.

## Building a prefix from VibeDarling branches and PRs

The launcher includes a versioned GPL snapshot of `all-vibedarling-pr-prefix.py`.
No Darling installation or external builder path is required. On startup without a
usable runtime, **Set up Darling** offers to build and install a private runtime or
select an existing one. Everything the launcher creates lives in `~/.darling-launcher`
(sources, workspaces, runtime images, prefixes, mount points and AI workspaces); this
location is fixed and not configurable.

Setup clones VibeDarling into `<data>/sources/vibedarling`, builds in a fresh
`<data>/workspaces/...` and automatically selects the resulting runtime/prefix.
No macOS volume is required for this build. Git, Python, CMake, Ninja and the
Darling host build dependencies must be available; errors retain the workspace
and appear in build progress. Compilation and private installation hold the
heavy-build lock. The launcher cannot close during an active clone/build.

The heavy-build lock is `$XDG_RUNTIME_DIR/darling-launcher/darling-heavy-build.lock`,
in a directory created 0700. `DARLING_LAUNCHER_LOCK_DIR` replaces that directory for
both the launcher's own builds and the agent's `build.sh`. Either way the directory
must be owned by you and not writable by group or others, otherwise the build stops.
To share the lock with other tools of yours that use `/tmp/agent-locks`, set
`DARLING_LAUNCHER_LOCK_DIR=/tmp/agent-locks` after making sure that directory is yours.

Builds always use default branches only (prebuilt CI releases replace the old master-plus-open-PRs build). Choose a job count and optional CMake settings (one `-DNAME=VALUE` per line). The script resolves main/master where present and records exceptions for repositories with another default branch.

The launcher snapshots the selected script and checks its CLI. It runs `resolve`, `checkout` and `build`, including `resolve-nested` and `checkout-nested` when that interface is available. For branch-only builds it creates separate top-level and nested locks with empty PR lists; original discovery locks are preserved. Existing workspaces and workspaces inside the source checkout are refused. Conflicts and script failures stop the workflow and retain the workspace for inspection.

The output prefix is `<workspace>/prefix`, the executable is `<workspace>/build/src/startup/darling`, and the install root is `<workspace>/image/usr/local`. The GUI selects these together after successful verification and sets `DARLING_INSTALL_PREFIX` for guest launches. Script, lock and integration-manifest hashes are recorded under the private prefix's `.darling-launcher/build-provenance.json`. No existing source checkout or installed runtime is modified by this integration. Keep the GUI open while imports and builds are active.

The builder requires GitHub network access and a full Darling build environment. The launcher never passes the builder's `--include-prs`, so open PRs are neither queried nor locked, and it strips any PR list a lock carries. The builder keeps `GITHUB_TOKEN`/`GH_TOKEN` out of every process it starts.

**Launch selected** uses `DPREFIX=<selected prefix> <selected darling> exec <guest executable>`. Each launch has a separate host `QProcess`, and the progress/output area shows its activity and exit status. If dyld reports `Symbol not found`, `Referenced from`, and `Expected in`, or an absolute `Library not loaded` path with `Reason: image not found`, the GUI offers to copy the named system library or framework from the selected mounted volume and retry. Each accepted import is added to a private chain at `<prefix>/.darling-launcher/catalog.json`. Loader errors are diagnosed as output arrives, even if the host wrapper remains running. **Stop selected prefix processes** explicitly shuts down all apps in that prefix before a retry. Library imports run in the background and record the source path and destination prefix. If the source has framework resources but no standalone library file, import stops with an explanation; extracting libraries from a dyld shared cache is outside this workflow. The proposed issue is saved locally under that directory for review. Nothing is submitted automatically.

**Install Brewfile** stages a copy under the prefix and runs only `/opt/homebrew/bin/brew` as a guest executable. Native guest Homebrew must already be installed there. Brewfiles with MAS entries are refused because Apple ID interaction needs a separate workflow. The Brewfile is never executed on Linux.

The **Opt in: source fix workflow** action opens the troubleshooting dialog described below. Source fixes use published sources, headers, documentation and API observations; completed source patches and PR drafts require explicit approval before submission.

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

**Create prefix** clones `https://github.com/VibeDarling/darling.git` into
`~/.darling-launcher/sources/vibedarling` when it is missing, streams Git progress and
builds in a new `~/.darling-launcher/workspaces/` folder. Existing checkouts are not
modified. Submodule integration remains the prefix builder's responsibility. The builder
script is the bundled snapshot until it is merged upstream. Clone failures retain
partial folders for inspection, and require an explicit retry.

### Apps, settings and contributions

The launcher opens directly to two matching app browsers. **Settings…** opens
configuration in a separate popup. The compact prefix selector shows where imports
and launches will run; its tooltip shows the full path. It lists previously used
prefixes and directories under the launcher's managed prefixes folder. Finish
imports and stop active prefix processes before switching with this selector.

Search each pane by app name. The available-app browser can filter to already
imported apps; the imported browser can filter to running or failed apps. Hidden
results are excluded from selection actions. Both panes support list/grid views,
Ctrl+A, Ctrl-click and Shift-click, and name or size sorting. The source volume,
prefix, runtime, view, sort, window geometry and pane sizes are remembered locally.
Empty panes explain how to choose a mounted source and import the first app.

Double-click an imported app or use **Launch selected**. Activity badges and text
show queued/importing, launching, running and failed states. Import progress counts
completed apps; it does not estimate byte-copy progress. Running means the host
launch process is alive, not proof that the guest window is usable. Drag imported
apps onto the trash icon to move their private copies into that prefix's
`.darling-launcher/trash` directory. Running apps cannot be trashed. **Undo** restores
session trash moves without overwriting an existing app; copies remain recoverable
on disk after closing the launcher. The macOS source is never deleted.

Before launching an app, choose recovery actions for missing dependencies: private
macOS library imports, an issue draft, or a background AI source fix. Recovery
preferences appear again for every failed launch unless remembered,
including process startup errors and failures without a diagnosed dependency.
Library retries within that attempt reuse its accepted choices.
The confirmation can remember your choices; change them in
**Settings → Launch recovery preferences…**.
The macOS import option appears only when the selected source matches a readable,
usable mounted volume. Otherwise **Mount or select macOS source…** opens the existing
explicit mount chooser and cancels the pending launch. If successful partition
discovery finds no APFS/HFS partition and no usable macOS mount is present, library
import stays visible but disabled with an explanation. Discovery errors retain the
mount chooser rather than claiming that macOS is absent. No partition is guessed.

A missing library or symbol opens an app-specific recovery popup. **Show details**
reveals loader output and provenance there; the main window keeps only app status
and progress. Import copies the exact standalone dependency into the selected
private prefix, records its source and retries with the original launch choices.
A dependency chain uses one set of choices: each newly diagnosed missing library
starts a separate AI session and workspace when AI is selected. Libraries are
resolved in loader order as retries reveal them; identical libraries do not start
duplicate sessions. The issue draft gathers the whole chain. Draft updates preserve
user edits and require an explicit refresh before approving a stale draft.
A stalled launch requires explicit prefix shutdown before importing. Copied-library
outcomes and contribution explanations remain in the popup.

Report prepares a completed editable issue draft targeting **VibeDarling/Darling**
for central triage. The target is fixed. The authenticated GitHub account
is used only after **Approve completed issue and submit**. Saving or selecting a
report option alone never submits it.

The popup detects installed Claude, Codex and OpenCode CLIs. Selecting AI in the
pre-launch confirmation authorizes sharing subsequent loader output and path
provenance with the selected tool’s configured service. Manual popup actions require
approval of the displayed data. The launcher creates an independent workspace,
saves diagnostic JSON and clean-room instructions, and runs the CLI in the background
with a private `AGENT.log`; a terminal option remains available. No Apple payloads are
attached. Jobs survive popup closure; Settings can stop the owned CLI. Tools retain
their configured authentication and permission boundaries. Source work uses independent
clones, private prefixes and the heavy-build lock. No push or issue/PR submission
is authorized by starting an agent.

If `gh auth status --hostname github.com` succeeds, **Review completed PR proposal…**
can load a local `proposal.json` with string fields `source`, `repo`, `base`, `head`,
`title`, and `body`. The source must be a clean independent clone inside the
proposal's workspace; target `repo` is `VibeDarling/<repository>`, `head` is
`<fork-owner>:<branch>`, and `base` is explicit. The complete text patch and draft
are displayed before an **Approve completed draft and submit PR** action. Binary
patches are refused. The published fork head must match the reviewed local commit.
Publishing that branch remains a separate explicitly approved action; the launcher
never pushes automatically. Tests use fake agents, Git and GitHub CLI, and never
submit real issues or PRs.

### Machine defaults and a private source build

With no saved prefix, the launcher proposes its own `prefixes/default` location
under Qt's application data directory. It does not select another tab's test
prefix. A verified installed launcher/runtime pair is populated automatically;
**Detected runtimes** offers other complete discovered build/image pairs. Saved
paths remain editable, and a prefix's build provenance takes priority when
recovering its runtime paths.

**Build and deploy Darling…** defaults a bundled builder script, a clean VibeDarling
main/master source clone (or a visible proposed clone path), and a fresh workspace.
First-run setup proposes its own home-managed independent source clone.
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
block devices, obtains APFS volume indices from filesystem metadata, mounts under
`/run/darling-launcher/<uid>/<device>-volume-<n>` (created by root, root-owned, so
nothing but root can re-point a mount point), and verifies device, target and
read-only flags afterwards; if that fails, only the helper's own mount point is
unmounted (lazily when busy). `/run/darling-launcher/<uid>` is root-owned, mode
0700, with a POSIX ACL that lets only your uid traverse it, so other users cannot
reach the mounted contents. This is one policy for the batch helper and the
cryptex script (`restrict_to_caller` in `tools/mount-macos.py`): where `/run` has no
ACL support both refuse to mount, because the mounts carry your primary group, which
may be shared, and Preboot is mounted with `allow_other`.
APFS FUSE runs with `default_permissions`, so the kernel enforces the `uid=`/`gid=`
ownership. Errors name the failing step, not file paths or tool output.
Progress and discovered application sources are emitted as JSON lines. Individual
driver failures are reported and the remaining detected volumes are attempted;
there is no authorization retry or alternate privilege mechanism.

Everything that runs as root must be a file only root can change: `pkexec`,
`mount` and `umount` are fixed under `/usr/bin`, Python is `/usr/bin/python3`,
`apfs-fuse` and `apfsutil` are taken only from `/usr/bin` or `/usr/local/bin`
(never from settings or arguments), and they and the script itself must be
root-owned and not group/other writable along their whole resolved path. A copy in a source or build
tree is refused, so install the helper where the launcher was configured to find
it (`LAUNCHER_MOUNT_SCRIPT`, by default `<prefix>/libexec/darling-launcher/mount-macos.py`):

```sh
sudo cmake --install build   # or:
sudo install -D -o root -g root -m 0755 tools/mount-macos.py /usr/local/libexec/darling-launcher/mount-macos.py
```

The installed script can also be run from your desktop terminal:

```sh
/usr/bin/python3 -I /usr/local/libexec/darling-launcher/mount-macos.py
```

The script uses the calling user's identity supplied by `pkexec` for file access
ownership. It never writes filesystem data to the macOS device or unlocks encrypted
volumes. Mounts remain present until explicitly unmounted. Apple payloads and
filesystem metadata are private local data and are not uploaded.

### Mount the cryptex OS image (dyld shared cache)

**Settings → Mount macOS source… → Mount cryptex OS (Preboot + os.dmg)** runs
`tools/mount-cryptex.py`, which automates two read-only mounts.

**Install first.** The scripts need Python 3.8 or newer, and root runs them with an interpreter that the launcher
picks when CMake configures it. There are three outcomes:

1. *Found.* The first of `/usr/bin/python3`, the interpreter `find_package(Python3)` found, `/usr/local/bin/python3`,
   then `/usr/bin/python3.8` to `/usr/bin/python3.20` that is root-owned along its whole path (a symlink needs root
   ownership and a trusted target), not group- or world-writable, runs, and reports 3.8 or newer is baked in.
   PATH and home-directory interpreters are never used.
2. *Explicit override.* `-DLAUNCHER_PYTHON=<path>` uses only that path under the same rule; if it fails, configuration
   stops with the reason (you asked for it).
3. *None found.* Without an override, configuration succeeds with a CMake warning that lists every candidate and why
   it was rejected, and bakes no interpreter. The launcher builds and runs, but the cryptex and Preboot buttons are
   disabled and the other mount buttons (mount all, mount selected) refuse when clicked, with the message "No trusted Python 3.8+ was
   found when the launcher was configured"; none of them reaches `pkexec`. Install a system Python (or pass `-DLAUNCHER_PYTHON`) and reconfigure. The script tests then run with
   the Python that `find_package` found, unprivileged.

The dialog and the script re-check the interpreter at run time. Root runs the script, so the launcher only uses a copy that only root
can change. `sudo cmake --install <build directory>` puts `mount-cryptex.py` and its helper
`mount-macos.py` (only those) into `<libexecdir>/darling-launcher` (default
`/usr/local/libexec/darling-launcher`; `-DLAUNCHER_MOUNT_CRYPTEX_SCRIPT=<path>` moves it).
Before any `pkexec` request, and again when it starts as root, the script checks that both
files and every parent directory (after resolving symlinks) are owned by root and not group-
or world-writable; the helper is compiled from that verified source instead of imported, so a
planted `__pycache__` is never used. Run from a source or build tree the action is disabled
and the dialog prints the install commands. `apfs-fuse` and `apfsutil` must also be installed
as root in `/usr/local/bin` or `/usr/bin` (same ownership rules); a copy in your home is used
only for step 2.

1. One `pkexec` request mounts the macOS Preboot volume. The privileged step takes no path
   from the launcher. Preboot is mounted at `/run/darling-launcher/<uid>/macos-preboot`, a
   chain created by root: `/run/darling-launcher` is 0755 and `<uid>` is `root:root` 0700 with
   a POSIX ACL entry that lets only your uid traverse it (the mount uses `allow_other` and
   holds FileVault user records, so other local users must not reach it). If the ACL cannot be
   set the action stops and mounts nothing. The command is
   `apfs-fuse -o allow_other,uid=<you>,gid=<you> -v <index> <partition> /run/darling-launcher/<uid>/macos-preboot`.
   The partition and volume index are found by running `apfsutil` on APFS partitions
   of real disks that no other driver has mounted (volumes of the same container that
   apfs-fuse already mounted, for example by the batch action, are fine) (loop devices and other non-disk parents are never examined, even when
   selected) and selecting the volume named `Preboot`. Select a partition to restrict the
   search to it. A Preboot mount whose apfs-fuse daemon has died is unmounted and replaced in
   the same request.
2. As you, without privileges, it mounts
   `<preboot>/root/<UUID>/cryptex1/current/os.dmg` with
   `apfs-fuse -o uid=<you>,gid=<you> -v 0 <os.dmg> <root>/macos-cryptex-os`, where
   `<root>` is the mount root: a directory below your home (a symlinked home prefix such
   as `/home -> /var/home` is resolved; symlinks below it are refused) or
   `/run/user/<uid>`. The `<UUID>` directory is found by listing; this mount has no
   `allow_other`.

When `<cryptex-os>/root/System/Library/dyld/` exists, its path is reported in the
dialog output. A missing tool, no Preboot volume, several Preboot volumes or images,
or an image path that resolves outside the Preboot mount stops the action with an
error; nothing is guessed. If Preboot is already mounted and responding, it is reused and no authorization is
requested.

`apfs-fuse` can use several GB of memory while it reads the cryptex image. Use
**Unmount cryptex OS** (no authorization needed), then **Unmount Preboot** (one `pkexec`
request; it unmounts only your `/run/darling-launcher/<uid>/macos-preboot` with the
root-owned `/usr/bin/umount`). The same script runs from a desktop terminal once installed:
`python3 -I /usr/local/libexec/darling-launcher/mount-cryptex.py --mount-root DIR --apfs-fuse PATH [--device /dev/...]`
(`--unmount` for Preboot). A polkit action pinned to the installed script is not provided; the
generic `pkexec` prompt names the Python interpreter.
The real mount and the `pkexec` prompt are not covered by the automated tests, which
use fixtures and fake commands.
## MCP server for AI clients

`tools/launcher-mcp.py` is a stdio [Model Context Protocol](https://modelcontextprotocol.io)
server that lets an AI client such as Claude Code inspect the launcher's state and, if you
opt in, start and stop apps. It uses only the Python 3 standard library, needs no network
and no Qt, and runs as its own process; nothing in the GUI changes. Register it with:

```sh
claude mcp add darling-launcher -- python3 -I /absolute/path/to/tools/launcher-mcp.py
# to allow launch_app, import_langid_data and stop_prefix as well:
claude mcp add darling-launcher -e DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1 -- python3 -I /absolute/path/to/tools/launcher-mcp.py
```

It reads `~/.darling-launcher` and the launcher's settings file (by default
`~/.config/cristim/darling-launcher.conf`; see the environment variables below).

### Environment variables

The server reads and forwards exactly these (it runs under `python3 -I`, which ignores
`PYTHON*`). A test (`test_the_environment_variables_the_code_uses_are_the_documented_ones`)
fails when the code touches a variable that is not in this table.

| Variable | What it affects | What the server trusts |
| --- | --- | --- |
| `HOME` | Required. Base of `~/.darling-launcher`, of the default settings path and of the `~` in redacted output. | Must be a normalized absolute path, otherwise the server refuses to start. |
| `DARLING_LAUNCHER_DATA_DIR` | Replaces `~/.darling-launcher` (prefixes, mounts, runtimes, logs). | Same check; every path the tools accept is confined below it. |
| `DARLING_LAUNCHER_CONFIG` | Replaces the settings file path. | Same check. Its contents select the `darling` that runs. |
| `XDG_CONFIG_HOME` | Base of the default settings path (`<it>/cristim/darling-launcher.conf`) when set. | Same check. |
| `DARLING_LAUNCHER_MCP_ALLOW_ACTIONS` | `1` enables `launch_app`, `import_langid_data` and `stop_prefix` (each call still needs `confirm: true`); `0` or empty disables them. | Any other value refuses to start. |
| `PATH` | Where `findmnt` is found, where `darling-launcher` is found for `import_langid_data`, and, when the settings name no `darling`, where `darling` is found (absolute entries only), so a changed `PATH` selects which program runs. Also forwarded to children. | The user's own `PATH`. `mount_plan` never takes a helper program from it. |
| `USER`, `LOGNAME`, `LANG`, `DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY`, `XDG_RUNTIME_DIR` | Not read by the server; forwarded unchanged to `darling` and `findmnt` so apps can open windows. | Passed through as they are. |

No other variable is read or forwarded: tokens, `DYLD_*` and everything else in the server's
environment never reach a child process. The server itself sets `DPREFIX` (the prefix) and,
when the settings name a runtime root, `DARLING_INSTALL_PREFIX` for `darling` children.

| Tool | Effect |
| --- | --- |
| `launcher_status` | Read-only. Installed runtime tags, selected paths, default prefix, mount root, last log lines. |
| `list_prefixes` | Read-only. Managed prefixes and their imported-app counts. |
| `list_apps` | Read-only. Apps in a prefix's import catalog. |
| `mount_status` | Read-only. `findmnt` entries under the launcher mount root. |
| `tail_log` | Read-only. Last lines of `launcher.log` or of the newest app this server started (`mcp-launch`). |
| `mount_plan` | Read-only. Returns the `pkexec`/`mount`/script command lines for you to run; runs nothing. |
| `launch_app` | Not read-only. `darling exec` of an imported app, with the `darling` selected in the launcher settings (or the first on `PATH`). Needs the opt-in and `confirm: true`. |
| `import_langid_data` | Not read-only. Copies `/usr/share/langid` from a macOS volume under the launcher mount root or `/run/darling-launcher/<uid>` into a managed prefix by running the first `darling-launcher` on `PATH` as `darling-launcher langid-import --volume V --prefix P` (the Import language data button's code, same checks). Needs the opt-in and `confirm: true`. |
| `stop_prefix` | Not read-only. `darling shutdown` for a prefix, with the same `darling`. Needs the opt-in and `confirm: true`. |

Safety model:

- Action tools are refused unless the server was started with
  `DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1`, and each call must also pass `confirm: true`.
- The server never calls `sudo` or `pkexec` and never mounts anything. `mount_plan` only
  returns commands, and it takes `pkexec`, `mount`, `umount`, `python3`, `apfs-fuse` and
  `apfsutil` only from root-owned files in `/usr/local/bin` or `/usr/bin` (the file and every
  parent root-owned and not group- or world-writable, as in the cryptex mount script). The
  caller cannot choose these paths; a missing tool is an error that says where to install it.
  The `batch` and `cryptex` plans run a launcher script that asks `pkexec` to run itself as
  root, so they are offered only when that script is installed root-owned in
  `/usr/local/libexec/darling-launcher` or `/usr/libexec/darling-launcher` (`sudo cmake --install
  <build directory>` puts it there); a copy in a checkout is never used, and until it is installed
  these two kinds return a warning and no command. The per-partition `pkexec mount` and `umount`
  plans need no script.
  The `batch` plan is the bare helper command: `mount-macos.py` takes no mount root and no tool paths,
  finds `apfs-fuse` and `apfsutil` in `/usr/bin` or `/usr/local/bin` itself and mounts under
  `/run/darling-launcher/<uid>`. If a tool is missing the plan warns; HFS-only machines still work.
  The `cryptex` plan passes `--mount-root`, `--apfs-fuse` and an optional `--device` to `mount-cryptex.py`.
- A prefix is a bare name or an absolute path of a direct, non-symlink child of
  `~/.darling-launcher/prefixes`. Mount directories must be inside
  `~/.darling-launcher/mounts` without symlink components. `..` is refused everywhere.
- Subprocesses run as argument lists (no shell) with a fixed environment allowlist, a
  timeout and a cap on captured output. A running app or a slow `darling shutdown` is left
  running; the server never sends signals to Darling or guest processes. Each `launch_app`
  writes its output to its own file under `~/.darling-launcher/logs/mcp-launch/` (mode 0600).
  The file has no size cap while the app runs. Files beyond the newest 20 are deleted only
  when they were last written more than an hour ago, so a running app keeps its log and
  recent logs are never pruned.
- Every result has home directory paths, `/home/<name>`-style user-name components and
  token-like strings redacted. In `mount_plan`, `argv` and the descriptions are redacted
  like everything else (home shown as `~`). Each command's `shell` string is the copy-paste
  form: it writes your home directory as `"$HOME"` (so paste it into your own shell), keeps
  every other path quoted, and shows a user-name component outside your home as the quoted
  placeholder `'<user>'` with a note telling you to replace it. Secrets are removed from
  `shell` too.
- Limits of redaction, which is a best-effort filter and not a guarantee: a single string is
  scanned up to 64 KiB (plus a 4 KiB lookahead); longer text is cut after redaction at a token
  boundary and ends with `[truncated before redaction]`. URL credentials
  (`scheme://user:password@host`) run to the last `@` of the whitespace-delimited token, so
  text after an earlier `@` in the same token is redacted too; `host:8080/...` and
  `bob:8080/x@y` read as a port, so a numeric password followed by `/` is not redacted;
  whitespace ends a token, so `pw @host` is not a credential. Labelled secrets separated only
  by whitespace are redacted when the next word is at least six characters, and a bare user
  name outside `/home/`, `/Users/` and `/run/media/` paths is never redacted.
- Protocol messages use stdout; logging goes to stderr. Lines over 256 KiB are rejected.

The tests (`ctest -R launcher-mcp`, or `python3 tests/launcher_mcp_test.py`) use temporary
directories and fake `darling`, `findmnt` and `pkexec` executables; they need no Darling,
privileges or network.

## Diagnostic log

The launcher appends to `~/.darling-launcher/logs/launcher.log` (rotated at 8 MB to a
timestamped file next to it). Each line is `time [source] text`. Sources: `app`, `build`
(clone and prefix builder output), `launch` (the exact darling command, loader output and
exit status), `agent` (AI fix sessions) and `qt-*` (Qt warnings). The path is shown in
Settings. To have Claude investigate a failure, ask it to read this file, plus the build
workspace named in the build status, and the loader output it records.
Logs hold local paths and loader output but never Apple binaries or symbol dumps.
