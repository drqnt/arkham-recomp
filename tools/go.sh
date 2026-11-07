#!/bin/zsh
# Rebuild, run to the engine with scripted Play, and summarize how far it got: tools/go.sh [seconds]
cd "$(dirname "$0")/.." || exit 1
[ -f config.local ] && . ./config.local  # local settings, not tracked: ARKHAM_RENDERER, ARKHAM_LANGUAGE, ARKHAM_METAL_HUD
[ "$ARKHAM_METAL_HUD" = 1 ] && export MTL_HUD_ENABLED=1  # Apple's Metal performance HUD in the game's window
if ! python3 -I tools/build.py > logs/build.log 2>&1; then
  grep -E 'FAILED|error:|failed=[1-9]|ld:|Undefined|"_' logs/build.log | head -20 | cut -c1-200
  exit 1
fi
export ARKHAM_STUB_ALL=1 ARKHAM_AUTO_ALERT=1 ARKHAM_AUTO_PLAY=${AUTO_PLAY:-10}
# File tracing writes tens of thousands of log lines under the runtime's lock, so it is only on when asked for: FS=1.
[ -n "$FS" ] && export ARKHAM_TRACE_FS=1
[ -n "$SNAP" ] && export ARKHAM_SNAP="$SNAP" ARKHAM_SNAP_AFTER="${SNAP_AFTER:-40}"
timeout "${1:-90}" "${ARKHAM_EXE:-build/Arkham.app/Contents/MacOS/arkham}" > logs/run.out 2> logs/run.err
echo "exit=$?"
sed_paths() { sed "s#$HOME/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY#GAME#; s#$HOME/Library/Application Support/Feral Interactive/Batman Arkham Asylum GOTY#FERAL#"; }
echo "--- last files:"; grep -a -E '^\[fs\] (open|fopen)' logs/run.err | tail -"${FILES:-5}" | sed_paths | cut -c1-110
echo "--- new stubs:"; grep -a '^\[stub\]' logs/run.err | sed 's/\[stub\] //; s/ -> 0//' | tail -"${STUBS:-14}" | tr '\n' ' ' | cut -c1-700; echo
grep -a -E '^\[(steam|gl|heap|snap|objc)\]' logs/run.err | grep -aE 'not bridged|no prototype|host has no|corrupt|non-heap|wrote|no visible|unsupported' | sort | uniq -c | sort -rn | head -"${NOTES:-8}" | cut -c1-120
grep -a -A3 '^\*\*\*' logs/run.err | head -4 | cut -c1-130
grep -a -A40 '^\*\*\* ' logs/run.err | grep -E ' F_| shim_' | sed -E 's/arkham +0x[0-9a-f]+ //; s/^[0-9]+ +//' | head -"${FRAMES:-12}" | tr '\n' ' ' | cut -c1-520; echo
