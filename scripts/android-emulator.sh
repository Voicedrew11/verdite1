#!/usr/bin/env bash
# Starts an emulated phone for the Android build (docs/ANDROID_PLAN.md), in the background:
#   nix develop .#android-emulator -c scripts/android-emulator.sh [--gpu MODE] [EMULATOR ARGS...]
# The device is created on first use, in ~/.android/avd. MODE is the emulator's -gpu: host (the
# host's GPU, by default) or swiftshader_indirect (software Vulkan). -no-window runs it unseen,
# for agents. Then
#   cmake --build --preset android-x86_64 && gradle -p android installDebug
set -euo pipefail
gpu=host
if [[ ${1:-} == --gpu ]]; then gpu=$2; shift 2; fi
name=verdite1-test
image="system-images;android-35;default;x86_64"
: "${ANDROID_HOME:?run in nix develop .#android-emulator}"
avdmanager="$(ls -d "$ANDROID_HOME"/cmdline-tools/*/bin | head -1)/avdmanager"
if ! "$avdmanager" list avd -c | grep -qx "$name"; then
  echo no | "$avdmanager" create avd --name "$name" --package "$image" --device pixel_7_pro
fi
exec "$ANDROID_HOME/emulator/emulator" -avd "$name" -gpu "$gpu" -no-snapshot -no-boot-anim -no-audio "$@"
