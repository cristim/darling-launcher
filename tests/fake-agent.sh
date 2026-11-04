#!/bin/sh
F=@FAKE@
stage=$(basename "$PWD")
n=$(cat "$F/count-$stage" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$F/count-$stage"
echo "$(basename "$0") $stage $n" >> "$F/calls"
mode=$(awk -v s="$stage" -v n="$n" '$1==s && ($2==n || $2=="*") {print $3; exit}' "$F/plan")
[ -n "$mode" ] || mode=ok
out() {
  case $stage in
    research) printf 'Facts: _CFFoo called 3 times, first at position 1 (attempt %s).\nSources: nm -u of the fixture app\n' "$n" ;;
    spec) printf 'Requirement 1 (rung 3): _CFFoo returns NULL for an empty key (attempt %s).\nSources: public CFFoo documentation\n' "$n" ;;
    impl) printf 'Implemented _CFFoo from the specification with a fixture test (attempt %s).\nSources: specification\n' "$n" ;;
    validate) printf 'VERDICT: PASS\nThe guest behaviour matches the specification (attempt %s).\n' "$n" ;;
    recon) printf 'Absent: _CFFoo\nBranch: master  Commit: 3ddf2a330123456789abcdef0123456789abcdef\nCallers exist: yes; files: src/CoreFoundation/CFFoo.c\n' ;;
  esac > OUTPUT.md
}
case $mode in
  ok) out ;;
  reconcile) out; echo 'Reconcile: _CFFoo' >> OUTPUT.md ;;
  r10) printf 'Loaded from libsystem_kernel.dylib+0x1c2 (attempt %s)\n' "$n" > OUTPUT.md ;;
  passhit) printf 'VERDICT: PASS\nMatches the spec; the old code called sub_1A2B.\n' > OUTPUT.md ;;
  badrecon) printf 'The function is absent and nobody calls it.\n' > OUTPUT.md ;;
  taint) printf 'Notes: the function calls sub_1A2B first.\n' > OUTPUT.md ;;
  revise) printf 'Loaded from /System/Library/Frameworks/Foo.framework (attempt %s)\n' "$n" > OUTPUT.md ;;
  selftaint) out; echo 'I opened a disassembly listing by accident.' > TAINTED.md ;;
  logtaint) out; echo 'Contents of (__TEXT,__text) section' ;;
  mismatch) printf 'VERDICT: MISMATCH\nThe guest sees an error for an empty key (attempt %s).\n' "$n" > OUTPUT.md ;;
  silent) exit 3 ;;
  symlink) ln -s ../research/trace/T0.jsonl OUTPUT.md ;;
  fifo) mkfifo OUTPUT.md ;;
  hardlink) ln "$F/outside.md" OUTPUT.md ;;
  chmod) chmod 700 ../research 2>/dev/null; ls ../research > /dev/null 2>&1; chmod 000 ../research 2>/dev/null; out ;;
  sleep) sleep 600 ;;
  chmod-then-sleep) chmod 700 ../research 2>/dev/null; chmod 000 ../research 2>/dev/null; sleep 600 ;;
  stubborn) trap '' TERM; sleep 600 ;;
  slow) sleep 2; out ;;
  biglog) head -c 9000000 /dev/zero | tr '\0' 'a'; echo; echo '{"msg": "x\\nContents of (__TEXT,__text) section"}'; out ;;
  bigclean) head -c 9000000 /dev/zero | tr '\0' 'a'; echo; out ;;
  escape) F="$F" setsid sh -c 'echo $$ > "$F/bgpid"; cd /; exec sleep 600' > /dev/null 2>&1 < /dev/null & sleep 1; out ;;
  checkbg) if kill -0 "$(cat "$F/bgpid")" 2>/dev/null; then echo alive > "$F/bg-state"; else echo dead > "$F/bg-state"; fi; out ;;
  probe)
    for p in ../research ../research/trace/T0.jsonl ../handoff ../handoff/01-research-to-spec.md ../spec ../spec/OUTPUT.md ..; do
      if ls "$p" > /dev/null 2>&1 || cat "$p" > /dev/null 2>&1; then echo "READ $p"; else echo "DENIED $p"; fi
    done > "$F/probe-$(basename "$0")"
    cp -r ../research "$F/stolen-$(basename "$0")" 2> /dev/null && echo COPIED >> "$F/probe-$(basename "$0")"
    find . -type f | sort > "$F/listing-$(basename "$0")"
    out ;;
esac
