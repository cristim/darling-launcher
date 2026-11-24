# Bundled upstream builder

`all-vibedarling-pr-prefix.py` is a GPL-3.0 source snapshot of the superproject
integration script from `https://github.com/VibeDarling/darling.git` (independent local clone
`/tmp/darling-all-prs-clean`, HEAD `72f129d13f81463d7227001890a6adc8f1244758`, last script change
`4aa1a123f4481ac27f2c47ed3e0663a36d76bc46`). The file name is kept for compatibility.

Launcher modifications (the only ones): `OWNER` and `ROOT` point at `darlinghq/darling`
(`https://github.com/darlinghq/darling.git`), the nested lock key is `nested` (was `vibedarling`),
and the descriptive labels and messages name darlinghq/darling. Its logic is otherwise unchanged.

Launcher snapshot SHA-256 (the bundled file, pinned by the GUI test):
`8471853f23be8bf4141b1803cf51a2c2783bb432e0118aff344cf30716dfd42e`.

Upstream script SHA-256 (before launcher modifications):
`be593c04751aa26e3b650d84442b62e2db8eb0b5888f4bc3cd281d4a850ae880`.
The source repository's GPL-3.0 license is compatible with this repository's GPL
license. The unmodified upstream script is preserved in its source repository; launcher-specific build locking and
input checks live in the host adapter. No Apple payloads are included.

Qt embeds this snapshot and copies it under the selected launcher data directory
with a content-hash filename. Explicit external script overrides remain supported.
Existing helper files, other checkouts and shared runtimes are not replaced.
