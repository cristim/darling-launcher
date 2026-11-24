# AGENTS.md (darling-launcher)

Qt 6 host launcher for user-owned Darling prefixes. It never runs inside a prefix. README.md describes behavior; PLAN.md records design decisions.

## Runtime: download when published, otherwise build only what is needed

- The upstream is `darlinghq/darling` (`LauncherReleases::repository`, the source clone `<data>/sources/darlinghq`). It publishes no prebuilt runtime yet, so `LauncherCore::setupPrebuilt` (`src/core.cpp`, `LauncherReleases::installLatest` in `src/releases.cpp`) reports that and the source build is the working path. A runtime that does get published lands in `~/.darling-launcher/runtimes/<tag>`.
- Building all of Darling from source (`PrefixDialog`, the bundled builder script) takes hours. Do not start it when a published download would do.
- When fixing an app failure, build only the component that contains the failing code (`build.sh build <target>` in the agent workspace) and deploy just its built files into the prefix. The agent prompt in `src/troubleshooting.cpp` says the same.
- The release contract (tag `vYYYY.MM.DD-<sha7>[-rN]`, `manifest.json` schema 1, asset `darling-runtime-<tag>-linux-<arch>.tar.zst`, attestation from `release-binaries.yml` on `refs/heads/master`) is the contract a publisher CI must meet (the VibeDarling CI, `scripts/ci/package_runtime.py`, is the reference). `darlinghq/darling` does not meet it yet, so do not treat the contract as verified against darlinghq. Change the launcher and any publisher CI together.

## Tests

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

- `launcher-prefix-tests` covers releases, discovery and the builder; `tests/release_fixture.h` is the shared fake `gh` release. `launcher-gui-tests` (offscreen) is slow, so run single tests while iterating: `./build/launcher-gui-tests <slot>`.
- Tests must never reach the network or the real `gh`/`git`: put fakes first on `PATH` and set `HOME` to a temporary directory.
- Builds are heavy. Use one job (`ninja -j1`) and your own project lock (`flock -w 1800 /tmp/agent-locks/darling-launcher-build.lock ...`), never a lock name another project could also use.

## Rules

- Clean-room: no Apple code, symbols dumps, disassembly or app/library payloads in this repository (see the Darling `AGENTS.md`).
- Nothing is submitted to GitHub (issues, PRs) without explicit user approval of the completed draft.
- Conventional commits; review each change before it is committed.
