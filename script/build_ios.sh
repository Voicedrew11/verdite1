#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
MODE="${1:-build}"
DEVICE="${2:-}"
TEAM="${KF_IOS_TEAM:-}"
if [[ "$MODE" != build && "$MODE" != install ]]; then
  echo "Usage: KF_IOS_TEAM=TEAM $0 [build|install DEVICE_ID]" >&2
  exit 2
fi
if [[ "$MODE" == install && ( -z "$DEVICE" || -z "$TEAM" ) ]]; then
  echo "Installing requires a device ID and KF_IOS_TEAM." >&2
  exit 2
fi
CMAKE_OPTIONS=("-DKF_IOS_TEAM=$TEAM")
if [[ -n "${KF_IOS_BUNDLE_ID:-}" ]]; then
  CMAKE_OPTIONS+=("-DKF_IOS_BUNDLE_ID=$KF_IOS_BUNDLE_ID")
fi
cmake --preset ios-device "${CMAKE_OPTIONS[@]}"
SIGNING=(CODE_SIGNING_ALLOWED=NO)
DESTINATION='generic/platform=iOS'
if [[ -n "$DEVICE" ]]; then DESTINATION="id=$DEVICE"; fi
if [[ -n "$TEAM" ]]; then
  SIGNING=(-allowProvisioningUpdates -allowProvisioningDeviceRegistration "DEVELOPMENT_TEAM=$TEAM")
fi
xcodebuild -project build/ios/kings_field.xcodeproj -scheme verdite1 \
  -configuration Release -destination "$DESTINATION" \
  -jobs "${KF_BUILD_JOBS:-6}" "${SIGNING[@]}" -quiet
APP="$ROOT_DIR/build/ios/Release-iphoneos/Verdite1.app"
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist")
echo "Built $APP"
if [[ "$MODE" == install ]]; then
  xcrun devicectl device install app --device "$DEVICE" "$APP"
  if [[ -n "${KF_IOS_ENGLISH_DATA:-}" ]]; then
    xcrun devicectl device copy to --device "$DEVICE" \
      --source "$KF_IOS_ENGLISH_DATA" --destination Documents/data/en \
      --domain-type appDataContainer --domain-identifier "$BUNDLE_ID"
  fi
  xcrun devicectl device process launch --device "$DEVICE" "$BUNDLE_ID"
fi
