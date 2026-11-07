#!/bin/zsh
# Puts the game on a connected iPhone or iPad: tools/ios_install.sh [device]
#  1. asks this Mac's Steam whether the account owns the game (arkham --steam-check; Steam must be running and signed in, offline mode is
#     fine) and copies the account's cloud saves, read only. Nothing goes further unless Steam says it's owned.
#  2. copies the game's Steam folder into the app's Documents, without the French, German, Italian, Spanish and Polish language packs
#     (0.7 GB of 10): Russian and English stay. Files already there and unchanged are skipped, so running this again is quick.
#  3. copies Steam's answer, and the cloud saves only if the device has none yet, so saves made on the device are never overwritten.
# The app must be on the device first: ios/app/run.sh [device] install-only. Nothing here changes the game's folder on the Mac.
cd "$(dirname "$0")/.." || exit 1
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
bundle=local.arkham-recomp.game
game="$HOME/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY"
device=$1
if [ -n "$device" ]; then
  id=$(xcrun devicectl list devices 2>/dev/null | grep -F -- "$device" | grep -v -i simulat | grep -i connected | head -1 | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -1)
else
  id=$(xcrun devicectl list devices 2>/dev/null | grep -E 'iPhone|iPad' | grep -v -i simulat | grep -w connected | head -1 | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -1)
fi
[ -n "$id" ] || { echo "No such device connected. Devices:"; xcrun devicectl list devices; exit 1; }
xcrun devicectl device info apps --device "$id" 2>/dev/null | grep -q "$bundle" || { echo "The app isn't on the device yet: ios/app/run.sh $device install-only"; exit 1; }
[ -d "$game/BMAAData" ] || { echo "No game at $game"; exit 1; }

echo "1. Asking Steam on this Mac"
[ -x build/Arkham.app/Contents/MacOS/arkham ] || python3 tools/build.py | tail -1
mkdir -p logs/ios
rm -rf logs/ios/steam
build/Arkham.app/Contents/MacOS/arkham --steam-check logs/ios/steam 2>/dev/null | tail -1
[ -f logs/ios/steam/answer.txt ] || { echo "No answer from Steam, so nothing was copied."; exit 1; }

echo "2. The game's files"
# Hard links in build/ios/stage stand for the files to copy, the language packs left out; they take no room and the originals stay as they are.
stage=build/ios/stage
rm -rf "$stage"
python3 - "$game" "$stage/Batman Arkham Asylum GOTY" <<'EOF'
import os, re, sys
src, dst = sys.argv[1], sys.argv[2]
skip = re.compile(r"_(FRA|DEU|ITA|ESN|POL)\.[A-Za-z0-9]+$")
skip_dirs = {"FRA", "DEU", "ITA", "ESN", "POL"}
n = size = 0
for root, dirs, files in os.walk(src):
    rel = os.path.relpath(root, src)
    if os.path.basename(root) in skip_dirs and "Localization" in rel:
        dirs[:] = []
        continue
    os.makedirs(os.path.join(dst, rel), exist_ok=True)
    for f in files:
        if f == ".DS_Store" or skip.search(f):
            continue
        p = os.path.join(root, f)
        os.link(p, os.path.join(dst, rel, f))
        n += 1
        size += os.path.getsize(p)
print(f"   {n} files, {size / 1e9:.1f} GB")
EOF
echo "   copying to the device; the first time takes a while"
xcrun devicectl device copy to --device "$id" --domain-type appDataContainer --domain-identifier "$bundle" \
  --source "$stage/Batman Arkham Asylum GOTY" --destination "Documents/Batman Arkham Asylum GOTY" 2>&1 | tail -2 | cut -c1-160
rm -rf "$stage"

echo "3. Steam's answer and the saves"
xcrun devicectl device copy to --device "$id" --domain-type appDataContainer --domain-identifier "$bundle" \
  --source logs/ios/steam/answer.txt --destination "Documents/Steam/answer.txt" 2>&1 | tail -1 | cut -c1-160
if xcrun devicectl device info files --device "$id" --domain-type appDataContainer --domain-identifier "$bundle" --subdirectory "Documents/Steam/cloud" 2>/dev/null | grep -q -E "Save|profile"; then
  echo "   the device has saves of its own: left as they are"
else
  xcrun devicectl device copy to --device "$id" --domain-type appDataContainer --domain-identifier "$bundle" \
    --source logs/ios/steam/cloud --destination "Documents/Steam/cloud" 2>&1 | tail -1 | cut -c1-160
fi
echo "Done. ios/app/run.sh starts the game."
