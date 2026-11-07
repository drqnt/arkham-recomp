#!/bin/zsh
# Run the translated game from this checkout. Steam must be running: it verifies ownership of app 35140.
# Debug switches: ARKHAM_TRACE_OBJC, ARKHAM_TRACE_FS, ARKHAM_TRACE_GL, ARKHAM_TRACE_DL, ARKHAM_JS_DEBUG,
# ARKHAM_FPS_CAP=n (frame rate limit, none by default), ARKHAM_AUTO_ALERT (answer dialogs), ARKHAM_AUTO_PLAY=secs (press Play), ARKHAM_SNAP=file.png ARKHAM_SNAP_AFTER=secs,
# ARKHAM_TRACE_PREFS (preferences read), ARKHAM_TRACE_IMPORTS=prefix (calls to imports with that prefix), ARKHAM_FAKE_PAD="secs:page:usage:value,..." (a pad with no hardware).
# Settings: ARKHAM_RENDERER=metal, ARKHAM_LANGUAGE=ru (or en, fr, de, it, es, pl).
cd "$(dirname "$0")/.." || exit 1
[ -f config.local ] && . ./config.local  # local settings, not tracked: ARKHAM_RENDERER, ARKHAM_LANGUAGE, ARKHAM_METAL_HUD
[ "$ARKHAM_METAL_HUD" = 1 ] && export MTL_HUD_ENABLED=1  # Apple's Metal performance HUD in the game's window
export ARKHAM_STUB_ALL=1  # imports without a shim return 0 for now; they are listed on stderr as [stub]
export ARKHAM_FPS=1       # on-screen frame rate counter
export ARKHAM_TRACE_HITCH=1  # logs what each thread was doing when a frame comes late; costs nothing otherwise
export ARKHAM_TRACE_PIPES=1  # logs the state of each draw that had to build a shader
if pgrep -x warm_shaders > /dev/null || pgrep -f -- --prebuild-shaders > /dev/null; then
  echo "Shaders are still being built ahead of play. Metal would give the game an empty cache file: wait until that has finished."
  exit 1
fi
built="$HOME/Library/Caches/arkham-recomp/built_for_samples"
engine_ini="$HOME/Library/Application Support/Feral Interactive/Batman Arkham Asylum GOTY/MacInit/BmEngine.ini"
if [ -s "$built" ] && [ -f "$engine_ini" ]; then
  samples=$(sed -n 's/^MaxMultisamples=\([0-9]*\).*/\1/p' "$engine_ini" | head -1)
  [ "${samples:-0}" = "$(cat "$built")" ] || echo "Shaders were built for another anti-aliasing level, so new places will stall. tools/build_shader_cache.sh rebuilds them."
fi
exec build/Arkham.app/Contents/MacOS/arkham "$@" 2> logs/run.err
