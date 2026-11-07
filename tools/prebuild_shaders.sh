#!/bin/zsh
# Compiles all the game's shaders into its Metal cache ahead of play, so that new places don't stall on them with ARKHAM_RENDERER=metal. Once is enough, a few minutes without a window,
# and the game must stay closed meanwhile. Everything it produces stays in the ignored logs folder and in ~/Library/Caches. Running it again only redoes what is missing.
cd "$(dirname "$0")/.." || exit 1
[ -f config.local ] && . ./config.local
if pgrep -x arkham > /dev/null; then
  echo "Close the game first: Metal gives each running process its own cache file, and the game wouldn't see this one's work."
  exit 1
fi
python3 -I tools/build.py > logs/build.log 2>&1 || { tail -5 logs/build.log; exit 1; }
python3 -I tools/check_shaders.py translate || exit 1
build/Arkham.app/Contents/MacOS/arkham --prebuild-shaders logs/msl/files.txt || exit 1
# Apple's Metal HUD changes the key Metal files compiled shaders under, so a game showing the HUD finds none of the above. Only a game whose HUD is up can fill that side: it opens,
# compiles behind its first logo for a few minutes and quits. Steam has to be running.
if [ "$ARKHAM_METAL_HUD" = 1 ]; then
  echo "Again for the Metal HUD: the game's window opens for a few minutes and closes by itself."
  MTL_HUD_ENABLED=1 ARKHAM_STUB_ALL=1 ARKHAM_AUTO_ALERT=1 ARKHAM_AUTO_PLAY=6 ARKHAM_PREBUILD_IN_GAME=logs/msl/files.txt build/Arkham.app/Contents/MacOS/arkham 2> logs/prebuild_hud.err
fi
