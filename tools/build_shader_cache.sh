#!/bin/zsh
# Builds the game's pixel shaders into Metal's disk cache ahead of play, so that new places don't stall the game. Once is enough; it takes about
# 10 minutes without a window, and the game must stay closed meanwhile. The first run also starts the game for a minute, so Steam must be running.
# Everything it produces stays in ~/Library/Caches. Running it again only redoes what is missing.
cd "$(dirname "$0")/.." || exit 1
if pgrep -f "^build/arkham" > /dev/null; then
  echo "Close the game first: Metal gives each running process its own cache file, and the game wouldn't see this one's work."
  exit 1
fi
python3 -I tools/build.py > logs/build.log 2>&1 || { tail -5 logs/build.log; exit 1; }
out="$HOME/Library/Caches/arkham-recomp"
mkdir -p "$out"
if [ ! -s "$out/programs.bin" ]; then
  game=$(python3 -I -c "import sys; sys.path.insert(0, 'tools'); from game_path import game_root; print(game_root())")
  caches="$game/BMAAData/BmGame/CookedPC"
  ARKHAM_SHADER_EXTRACT="$out/programs.tmp" ARKHAM_SHADER_SOURCES="$caches/GlobalShaderCache-PC-D3D-SM3.bin;$caches/RefShaderCache-PC-D3D-SM3.upk" tools/walk.sh 280 "" > /dev/null
  if ! grep -a -q '^\[extract\] done' logs/run.err; then
    echo "The game didn't get as far as its shaders: see logs/run.err"
    exit 1
  fi
  mv "$out/programs.tmp" "$out/programs.bin"
fi
# Shaders depend on the anti-aliasing level, so changing it in the game's settings means running this again.
engine_ini="$HOME/Library/Application Support/Feral Interactive/Batman Arkham Asylum GOTY/MacInit/BmEngine.ini"
samples=$(sed -n 's/^MaxMultisamples=\([0-9]*\).*/\1/p' "$engine_ini" | head -1)
echo "Building for anti-aliasing level ${samples:-0}"
build/warm_shaders "$out/programs.bin" "${samples:-0}" || exit 1
echo "${samples:-0}" > "$out/built_for_samples"
