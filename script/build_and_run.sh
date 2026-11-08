#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-run}"
if [[ $# -gt 0 ]]; then shift; fi
case "$MODE" in
  run|--debug|--logs|--telemetry|--verify|--build-only) ;;
  *) echo "usage: $0 [run|--debug|--logs|--telemetry|--verify|--build-only] [game arguments...]" >&2; exit 2 ;;
esac
cd "$ROOT_DIR"
APP_BUNDLE="$ROOT_DIR/dist/Verdite1.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/verdite1"
LOG_FILE="$ROOT_DIR/build/macos-run.log"

# Stop only this project's bundle before replacing its files.
pkill -f "^${APP_BINARY}([[:space:]]|$)" >/dev/null 2>&1 || true
for attempt in {1..30}; do
  if ! pgrep -f "^${APP_BINARY}([[:space:]]|$)" >/dev/null; then break; fi
  sleep 0.1
done
if pgrep -f "^${APP_BINARY}([[:space:]]|$)" >/dev/null; then
  echo "Verdite1 is still running. Close its window or error dialog before rebuilding." >&2
  exit 1
fi
cmake --preset macos
cmake --build --preset macos --target verdite1 --parallel "${KF_BUILD_JOBS:-6}"
python3 scripts/package_macos.py
case "$MODE" in
  --build-only) exit 0 ;;
  --debug) exec lldb -- "$APP_BINARY" "$@" ;;
esac
LAUNCH_ENV=()
for name in KF_AGENT KF_SHELL KF_TEST_NO_PAUSE; do
  if [[ -n "${!name:-}" ]]; then LAUNCH_ENV+=(--env "$name=${!name}"); fi
done
/usr/bin/open -n "${LAUNCH_ENV[@]}" --stdout "$LOG_FILE" --stderr "$LOG_FILE" "$APP_BUNDLE" --args "$@"
case "$MODE" in
  --verify) sleep 3; pgrep -f "^${APP_BINARY}([[:space:]]|$)" >/dev/null ;;
  --logs) exec tail -f "$LOG_FILE" ;;
  --telemetry) exec /usr/bin/log stream --info --style compact --predicate 'process == "verdite1"' ;;
esac
