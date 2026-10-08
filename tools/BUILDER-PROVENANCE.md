# Bundled VibeDarling builder

`all-vibedarling-pr-prefix.py` is an unchanged GPL-3.0 source snapshot from
`https://github.com/VibeDarling/darling.git`, independent local clone
`/tmp/darling-all-prs-clean`. Clone HEAD at capture:
`72f129d13f81463d7227001890a6adc8f1244758`. Last script change:
`4aa1a123f4481ac27f2c47ed3e0663a36d76bc46`.

Original script SHA-256:
`be593c04751aa26e3b650d84442b62e2db8eb0b5888f4bc3cd281d4a850ae880`.
The source repository's GPL-3.0 license is compatible with this repository's GPL
license. The original script is preserved; launcher-specific build locking and
input checks live in the host adapter. No Apple payloads are included.

Qt embeds this snapshot and copies it under the selected launcher data directory
with a content-hash filename. Explicit external script overrides remain supported.
Existing helper files, other checkouts and shared runtimes are not replaced.
