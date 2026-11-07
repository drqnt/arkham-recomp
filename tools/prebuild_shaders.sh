#!/bin/zsh
# Compiles all the game's shaders into its Metal cache ahead of play, so that new places don't stall on them with ARKHAM_RENDERER=metal. Once is enough, a few minutes without a window,
# and the game must stay closed meanwhile. Everything it produces stays in the ignored logs folder and in ~/Library/Caches. Running it again only redoes what is missing.
cd "$(dirname "$0")/.." || exit 1
if pgrep -x arkham > /dev/null; then
  echo "Close the game first: Metal gives each running process its own cache file, and the game wouldn't see this one's work."
  exit 1
fi
python3 -I tools/build.py > logs/build.log 2>&1 || { tail -5 logs/build.log; exit 1; }
python3 -I tools/check_shaders.py translate || exit 1
exec build/Arkham.app/Contents/MacOS/arkham --prebuild-shaders logs/msl/files.txt
