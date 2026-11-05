#!/bin/zsh
# Run the translated game from this checkout. Steam must be running: it verifies ownership of app 35140.
# Debug switches: ARKHAM_TRACE_OBJC, ARKHAM_TRACE_FS, ARKHAM_TRACE_GL, ARKHAM_TRACE_DL, ARKHAM_JS_DEBUG,
# ARKHAM_AUTO_ALERT (answer dialogs), ARKHAM_AUTO_PLAY=secs (press Play), ARKHAM_SNAP=file.png ARKHAM_SNAP_AFTER=secs.
cd "$(dirname "$0")/.." || exit 1
export ARKHAM_STUB_ALL=1  # imports without a shim return 0 for now; they are listed on stderr as [stub]
exec build/arkham "$@" 2> logs/run.err
