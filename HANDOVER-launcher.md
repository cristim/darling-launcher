# Handover: darling-launcher (Qt host launcher)

Repo: /home/cristi/src/darling-launcher (cristim/darling-launcher), branch main.

## Task
Qt host-side launcher: prefixes, stock-app import, launch, loader-only diagnosis, library import, VibeDarling issue and clean-room PR drafts (never submitted without explicit user approval). Moving to CI-built prebuilt runtimes.

## Done (all local, NOT pushed; origin/main is 3 behind)
- 248cc0e discover prebuilt runtimes under ~/.darling-launcher/runtimes/<tag>
- 84c2487 default-branches-only builds (open-PR input and --no-prs removed)
- afdcf56 release tag parser + latest-asset selection (src/releases.*), tags YYYY-MM-DD-HH-MM-<sha> UTC, assets darling-<component>-<arch>.tar.zst
- bc3bfdb LauncherReleases::installArchive: SHA-256 check, tar member vetting (no abs/.. paths, no hard links/devices, no write through symlink), extract to <tag>.partial then rename; test runtimeArchiveInstall
- Earlier pushed: security hardening, Verify/Repair/Darling buttons, ff-only clone update.

## In progress / not started
- Download of the release asset (GitHub API listing -> asset URL), `gh attestation verify`, soname preflight, download UI (prebuilt first, build second), new AGENTS.md, AI-agent prompt update in src/troubleshooting.cpp (download by default; build only the relevant component).
- Validate prefix after creation using checkPrefix.

## Blockers
- Push approval for 4 local commits.
- Real source build blocked on mslc/darling-metal upstream (cristim/mslc#201, darling-metal#28, VibeDarling/darling src/external/metal bump; sync VibeDarling/mslc master).
- User decisions pending: pkexec helper lookup in discovery.cpp, shared /tmp lock path, optional bubblewrap sandbox for the agent.
- Leftover dirs (user decides, nothing deleted): ~/.darling-launcher/workspaces/darling-workspace (4.9G superseded), darling-workspace-2 (mslc failure evidence).
- SwiftUI slices: on HOLD, I hold no CLAIMS rows.

## Next steps
1. Get push approval; push; tell ci-binaries agent: no --no-prs, tags YYYY-MM-DD-HH-MM-<sha>, assets darling-<component>-<arch>.tar.zst, no manifest.json.
2. Add a releases fetch (gh api repos/<repo>/releases) feeding LauncherReleases::latest, then installArchive; run `gh attestation verify`.
3. Wire UI + AGENTS.md + prompt update.
Build under `flock -w 1800 /tmp/agent-locks/darling-heavy-build.lock`, jobs 1.

## Evidence
~/.darling-launcher/logs/launcher.log; tests: build/launcher-prefix-tests; transcript /home/cristi/.claude/projects/-home-cristi-src-darling-launcher/6b382947-94d0-4f09-bde8-c0b8f16b0247.jsonl
