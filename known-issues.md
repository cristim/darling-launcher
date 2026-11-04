# Known issues

## Missing-symbol pipeline (src/pipeline*.cpp, src/handoff*.cpp)

- Isolation is same-user: directory modes keep out honest mistakes and chmod-and-restore is detected through
  ctime, but an agent that ignores its brief can read anything the user can read. Between stages, while the
  launcher checks and copies files, no folder is locked.
- Codex and OpenCode keep session transcripts in the user's home folder; only Claude gets a no-persistence
  flag. Research-stage content can therefore persist outside the run folder.
- The trace stays where the tracer wrote it; the pipeline works on a copy in research/trace.
- The audit chain is unkeyed. Deleting the final record together with its file, or rewriting the whole chain
  consistently, is caught only against a `records=... last_hash=...` line recorded elsewhere (the launcher
  keeps it in memory for the life of a run only).
- impl->spec questions are allowed by the store but not routed by the state machine.
- The implementation stage gets only its own folder; giving it a Darling worktree and build tools is stage 3.
- BackgroundFix (the existing source-fix agent) does not run under the stage supervisor; migrate it.
- The trace allow-list in PipelineWorkspace::importTrace guesses the tracer's JSONL keys until the tracer's
  schema is merged; a key outside it refuses every trace. `regs`/`words` values cannot be told apart from
  stack words: that depends on the tracer following the ruling (no stack capture in v1).
- A process left in a run folder without the agent marker (or one started through systemd or a terminal
  multiplexer) is not killed; the run stops and names it.
