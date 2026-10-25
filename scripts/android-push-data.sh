#!/usr/bin/env bash
# Copies the extracted game files (data/en, and data/ja when present) into the debug app's user
# folder on the connected phone, where importing a disc image puts them, so the game starts
# without the file picker. Needs the debug APK installed (run-as works on debug builds only).
#   nix develop .#android -c scripts/android-push-data.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
package=io.github.voicedrew11.verdite1
languages=()
for language in en ja; do
  [[ -f "$root/data/$language/GAME.EXE" ]] && languages+=("data/$language")
done
if (( ${#languages[@]} == 0 )); then
  echo "no game files in $root/data/en or data/ja" >&2
  exit 1
fi
tar -C "$root" -cf - "${languages[@]}" | adb exec-in "run-as $package sh -c 'mkdir -p files && tar -xf - -C files'"
adb shell "run-as $package ls files/data"
