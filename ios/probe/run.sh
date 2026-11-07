#!/bin/zsh
# Builds the probe, puts it on a connected device and prints what it finds: ios/probe/run.sh "device name or identifier" [entitlements]
# The entitlements for more memory and address space need a paid developer account.
# Needs an Apple account in Xcode, and Developer Mode on the device. The results also stay in logs/probe_<device>.txt.
cd "$(dirname "$0")" || exit 1
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
device=${1:?which device}
# The team's identifier stays out of the repository, in team.local next to this script. Xcode shows it under Signing & Capabilities.
team=$(cat team.local 2>/dev/null)
[ -n "$team" ] || { echo "No team yet: put its identifier in ios/probe/team.local."; exit 1; }
id=$(xcrun devicectl list devices 2>/dev/null | grep -F "$device" | grep -v -i simulator | head -1 | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -1)
[ -n "$id" ] || { echo "No such device. Connected:"; xcrun devicectl list devices; exit 1; }
extra=()
[ "$2" = "entitlements" ] && extra=(CODE_SIGN_ENTITLEMENTS=Probe.entitlements)
out=${TMPDIR:-/tmp}/arkham-probe-build
xcodebuild -project Probe.xcodeproj -scheme Probe -configuration Release -destination "id=$id" -derivedDataPath "$out" -allowProvisioningUpdates -allowProvisioningDeviceRegistration DEVELOPMENT_TEAM="$team" "${extra[@]}" build 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | cut -c1-200
app="$out/Build/Products/Release-iphoneos/Probe.app"
[ -d "$app" ] || exit 1
xcrun devicectl device install app --device "$id" "$app" 2>&1 | tail -1 | cut -c1-120
name=${device// /_}
log="../../logs/probe_$name.txt"
: > "$log"
xcrun devicectl device process launch --console --terminate-existing --device "$id" local.arkham-recomp.probe >> "$log" 2>&1 &
pid=$!
# The app stays open after its last test, so stop waiting when it says it is done.
seen=0
for i in {1..300}; do
  n=$(wc -l < "$log")
  if [ "$n" -gt "$seen" ]; then
    tail -n +$((seen + 1)) "$log" | grep -a '^\[probe\]' | sed 's/^\[probe\] //' | cut -c1-130
    seen=$n
  fi
  grep -q '^\[probe\] done' "$log" && break
  kill -0 $pid 2>/dev/null || { echo "the app ended before finishing"; tail -5 "$log" | cut -c1-200; break; }
  sleep 2
done
kill $pid 2>/dev/null
