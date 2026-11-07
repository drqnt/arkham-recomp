#!/bin/zsh
# Builds the game app, puts it on a connected iPhone or iPad and starts it, with its log on this Mac: ios/app/run.sh [device] [build-only|install-only]
# build-only stops after building, install-only after putting it on the device.
# The device is a name or identifier from `xcrun devicectl list devices`; without one, the first connected iPhone or iPad.
# The game's files must be on the device already: tools/ios_install.sh. The run's log is logs/ios/run.txt, and the app also keeps it in its Documents/Logs.
# Needs the Apple account in Xcode (the free Personal Team will do, its apps run for 7 days) and Developer Mode on the device.
cd "$(dirname "$0")" || exit 1
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
root=$(cd ../.. && pwd)
# The team's identifier stays out of the repository, in team.local here or next to the probe. Xcode shows it under Signing & Capabilities.
team=$(cat team.local 2>/dev/null || cat ../probe/team.local 2>/dev/null)
[ -n "$team" ] || { echo "No team yet: put its identifier in ios/app/team.local."; exit 1; }
device=$1
if [ -n "$device" ]; then
  id=$(xcrun devicectl list devices 2>/dev/null | grep -F -- "$device" | grep -v -i simulat | grep -i connected | head -1 | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -1)
else
  id=$(xcrun devicectl list devices 2>/dev/null | grep -E 'iPhone|iPad' | grep -v -i simulat | grep -w connected | head -1 | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -1)
fi
[ -n "$id" ] || { echo "No such device connected. Devices:"; xcrun devicectl list devices; exit 1; }
bundle=local.arkham-recomp.game
out=${TMPDIR:-/tmp}/arkham-app-build
app="$out/Build/Products/Release-iphoneos/Arkham.app"

python3 "$root/tools/build.py" --ios | tail -1 || exit 1
python3 -I "$root/tools/ios_icon.py" || exit 1  # the icon, from the game's own on this Mac, into build/ios
rm -rf "$app"
xcodebuild -project Arkham.xcodeproj -scheme Arkham -configuration Release -destination "id=$id" -derivedDataPath "$out" -allowProvisioningUpdates \
  -allowProvisioningDeviceRegistration DEVELOPMENT_TEAM="$team" build 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | cut -c1-200
[ -d "$app" ] || exit 1
[ "$2" = "build-only" ] && exit 0
xcrun devicectl device install app --device "$id" "$app" 2>&1 | grep -E "App installed|rror" | head -3 | cut -c1-160
[ "$2" = "install-only" ] && exit 0

mkdir -p "$root/logs/ios"
log="$root/logs/ios/run.txt"
: > "$log"
echo "Started on the device; its log goes to logs/ios/run.txt. Ctrl-C here stops watching, the game keeps running."
xcrun devicectl device process launch --console --terminate-existing --device "$id" "$bundle" >> "$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' INT TERM
seen=0
while kill -0 $pid 2>/dev/null; do
  n=$(wc -l < "$log")
  if [ "$n" -gt "$seen" ]; then
    tail -n +$((seen + 1)) "$log" | grep -a -E '^\[(ios|rt|mtl|steam|fps)\]|\*\*\*|missing import|ended' | cut -c1-160
    seen=$n
  fi
  sleep 2
done
tail -n +$((seen + 1)) "$log" | grep -a -E '^\[(ios|rt|mtl|steam|fps)\]|\*\*\*|missing import|ended' | cut -c1-160
echo "The app ended; the last lines of its log:"
tail -15 "$log" | cut -c1-200
