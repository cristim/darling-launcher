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
