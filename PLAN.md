# Darling Launcher implementation plan

1. Build a host Qt application with explicit Darling executable, mounted volume, and prefix fields. Without these, imports or launches could target an unintended installation.
2. Discover `.app` bundles under the selected volume's `Applications` and `System/Applications`, including subdirectories but excluding app interiors; copy only selected bundles into the chosen prefix, rejecting symlinks and path escapes. Store a private local catalog in the prefix.
3. Run each app with its own `QProcess` and `DPREFIX`, stream output and status, and parse dyld's unresolved symbol and expected library from loader output. Offer a confined import from that exact volume path, retry, and append the chain to the local catalog.
4. Stage a selected Brewfile in the prefix and invoke guest Homebrew only when its executable exists. Show process output and exit status.
5. Generate a local proposed issue from the chain; give the user a separate opt-in source-fix checklist. No network submission is automatic.
6. Test parser and copy confinement with synthetic fixtures; build and launch the actual Qt GUI in an offscreen display.

No changes are planned in the Darling or VibeDarling checkouts. Provenance: Darling `AGENTS.md` clean-room rule, `src/startup/darling.c` DPREFIX and exec interface, and `tools/darling-applications` in the integration checkout for prefix/Brewfile conventions.

Follow-up slice: explicit read-only pkexec mounting, partition discovery, and a metadata/icon/size browser with list/grid selection and drag-to-import. Verify with fake privilege commands and synthetic ICNS fixtures; real mounting waits for a user-selected partition and installed APFS driver.

Prefix-builder integration: use the existing committed external tool's resolve/checkout/build interface in a new workspace, preserving discovery and selected locks. Expose default branches or default branches plus PRs. Select the resulting private runtime and prefix together and test both scope choices, failure stops and runtime environment propagation with a synthetic builder.

Mounted-source reuse: read the live mount table including generic FUSE entries;
map every mounted volume to its selected partition and validate readable app/library
directories, including APFS FUSE's root wrapper. Without this, existing mounts are
hidden or the wrong volume is chosen. Show unsuitable mounts separately from usable
sources, require a choice when several are usable, and refresh without mounting.
Prove partition mapping, confinement, automatic/manual refresh and GUI import with
synthetic fixtures; run the real desktop GUI against the existing mounted volumes.

Launcher simplification: move settings to a hidden popup; reuse AppBrowser for
both source and imported apps so list/grid styles and icons match. Double-click
launches an imported app. Dropping onto trash moves only catalogued, non-running
app copies into the selected prefix's private trash, with rollback on catalog
failure. Show loader logs and troubleshooting tools only for failed apps.
Agent choices launch installed CLIs only after the user chooses one, with local
loader/provenance data and clean-room instructions in a new workspace. GitHub
submission requires authenticated gh plus explicit approval of a completed PR
proposal and its source patch. Verify fake tools/mounts and real host GUI; preserve
shared runtime, encrypted volumes and other source checkouts.

Eight UX improvements, in requested order:
1. Search each browser and filter source imports / running / failed apps; verify hidden items cannot be imported via select-all.
2. Show app state and progress in list/grid tiles; derive state from owned process/import events.
3. Summarize failures and put raw selected-app logs behind Show details.
4. Offer Undo for prefix-local trash moves; validate restoration and never overwrite an existing app.
5. Add a compact known-prefix selector showing the import/launch target.
6. Persist view, sorting, geometry, splitter and selected source/prefix; verify restoration.
7. Guide empty panes toward source selection and first import, including no-mounted-volume state.
8. Explain local dependency imports, issue drafts and agent fixes separately, preview shared data and require explicit consent before starting an agent.
For every step, add behavior tests; build under the shared heavy-build lock and run the real Qt GUI.

Missing dependency popup: diagnose loader output once per current failure and show
an app/prefix-bound popup with collapsed details and independent import, completed
issue review, and background AI checkboxes. Move main-window failure details and
workaround explanations into this popup. Keep imports bound to the originating
prefix and source; a stalled launch requires an explicit prefix shutdown before
retry. A report checkbox prepares a completed editable draft; only its explicit
approval submits via authenticated gh. Background CLI jobs use supported batch
interfaces, new workspaces and private logs; closing the popup must not stop them.
Test automatic popup/deduplication, all-checkbox actions, consent, issue cancellation
and approval, background lifecycle, missing tools and prefix changes. Run the actual
GUI with existing read-only mounts; never start real agents or submit real reports
as part of verification.

Pre-launch recovery preferences: ask before each manual launch unless the user
remembers import/report/background AI choices; Settings edits the same stored
choices. Retries inherit the original choices. AI consent explicitly covers future
loader/path provenance; issue/PR drafts always require their own completed-draft
approval. Never start a different agent if the remembered one is unavailable.

Mounted-source gating: match the selected canonical source against readable usable
mount candidates. Hide macOS import options without a verified mount and offer the
existing explicit mount/source chooser. Selecting that chooser cancels a pending
launch. Refresh popup availability when paths/mounts change; preserve import errors
across mount refresh. Verify synthetic mount loss, remembered automatic copy/retry,
pre-launch confirmation, fake background agents and issue approval. Use the actual
read-only p2 System mount and owned temporary Calculator prefix for desktop checks.

Failure preferences: show recovery choices once per failed launch attempt (including
loader errors emitted before wrapper exit, generic failures, invalid runtime paths,
and process startup errors), unless the user remembered preferences. Gate automatic
actions until failure choices are accepted; deduplicate streaming/exit notifications.
Preserve explicit completed-draft approval for every GitHub submission.

No dual-boot source: asynchronously reuse the existing lsblk partition parser. If
successful discovery finds no APFS/HFS partition and no usable macOS mount exists,
show library import disabled with an explanation. If partitions exist but none are
usable/mounted, offer the existing explicit mount chooser. Keep unknown discovery
results distinct from confirmed absence. Mounted sources override absent disk results.

Central triage: always target completed issue drafts and approved issue submissions
at VibeDarling/Darling; display the target read-only. Verify exact target/body with
fake gh and retain separate approval of completed issue and PR drafts.

Multi-library recovery: keep one accepted action set throughout automatic dependency
retries. Resolve libraries in loader order; start one independent background AI job
and workspace for each new missing library, deduplicating repeated diagnoses. Manual
popup action selection applies to subsequent dependencies too. Group reporting into
one central issue draft that tracks the chain; preserve user edits, mark stale drafts
and require explicit refresh/review before submitting changed provenance.

First-run runtime setup: offer private build/install or existing runtime selection
when the selected executable/private image is unavailable. Use one configurable
home data folder for sources, build/image/prefix workspaces, mounts and agent
workspaces. Changing that folder never moves existing user data.

Embed the ready separate-tab GPL builder with exact commit/hash provenance;
external overrides remain optional. Protect clone/build lifetime and hold the
shared heavy-build lock for compilation/private installation. Verify synthetic
first-run clone/build, path adoption, embedded helper CLI/hash and real Wayland UI.

Fixed data location: all launcher data lives in ~/.darling-launcher (not configurable).
Create prefix has no script/source/workspace fields: source clone is
<data>/sources/vibedarling (auto-cloned), workspace is generated per build, script is the
bundled snapshot. Removed the dataRoot setting, the --prefix-builder CLI option and the
tool/source detection and browse buttons. Pending: builder script moves into VibeDarling
upstream via a PR from its author; the launcher will fetch it from that PR until merged.

Settings simplification: Settings keeps only the mounted macOS volume (auto-detected,
with a chooser for several mounted volumes), mount/detect buttons, recovery preferences
and stop-agents. Prefix, Darling executable, runtime root and detected-runtime fields
are internal state (auto-detected, or set by build/first-run "Select an existing
runtime" file dialog). Mount dialog: APFS helper paths, batch root and mount directory
are automatic. Agent source-fix workspace is generated, not edited.

Background builds: while cloning/building, "Continue in the background" (or closing the
build dialog or the main window) hides the windows; both reappear when the clone or build
finishes or fails. Quitting remains blocked until then (closing only hides).

Diagnostic log: ~/.darling-launcher/logs/launcher.log (build output, launch commands and
loader output, agent output, Qt messages), path shown in Settings, README documents it.
First run: "I already have a Darling checkout and prefix..." takes a checkout root (must
contain build/src/startup/darling and a runtime image) and an optional existing prefix.

Drag selection: presses on an app icon/name keep the selection and start a drag after the
drag distance (selection is not redone); presses on blank space rubber-band. Double-click
is handled explicitly. File-manager drops: URL drops of .app folders under the selected
macOS volume import by their volume-relative path; anything else is refused with a message.

First run: when no runtime is found the launcher starts the clone/build/prefix setup by itself
in the background (hidden build dialog; banner explains the wait, offers hide and "I already
have Darling"). Apps dropped meanwhile are queued, shown grayed out, and import when the
prefix is ready; a desktop notification (notify-send) and the window reappear when done or
failed. Scan/Stop/Brewfile buttons sit below the panels; the app lists have a Size column and
clickable Name/Size headers (replacing the sort dropdown).
