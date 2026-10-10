# Bundled VibeDarling builder

`all-vibedarling-pr-prefix.py` is an unchanged GPL-3.0 source snapshot from
`https://github.com/VibeDarling/darling.git`, independent local clone
`/tmp/darling-all-prs-clean`. Clone HEAD at capture:
`72f129d13f81463d7227001890a6adc8f1244758`. Last script change:
`4aa1a123f4481ac27f2c47ed3e0663a36d76bc46`.

Original script SHA-256:
`be593c04751aa26e3b650d84442b62e2db8eb0b5888f4bc3cd281d4a850ae880`.
The source repository's GPL-3.0 license is compatible with this repository's GPL
license. Launcher-specific build locking and input checks live in the host
adapter. No Apple payloads are included.

Launcher changes to the snapshot (the hash above is the unmodified original; the
modified file's SHA-256, pinned by the GUI test, is
`6b8f5cff62e23637cbcaaeddbc2eac961d77604da88b35b906ce0a1e90326939`):

- `resolve` and `resolve-nested` lock open PR heads only with `--include-prs`;
  the default is default branches only, so PR code is never built unless the
  user selects it. The launcher passes the flag only for that choice.
- Every child process (git, cmake, ninja, the built runtime and guest) gets an
  environment built from an allow-list (`CHILD_VARIABLES` plus `LC_*`) with
  `GIT_TERMINAL_PROMPT=0`; tokens, credentials and `GIT_CONFIG_*` stay out. Only
  the in-process GitHub API client reads `GITHUB_TOKEN`.

Qt embeds this snapshot and copies it under the selected launcher data directory
with a content-hash filename. Explicit external script overrides remain supported.
Existing helper files, other checkouts and shared runtimes are not replaced.
