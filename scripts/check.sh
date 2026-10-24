#!/usr/bin/env bash
# Before a commit: build, lint our code, run the tests, then the sanitizer build's tests and a
# smoke run (boot headless, walk, quit; then the same drawing with Vulkan offscreen) under ASan and
# UBSan. Stops at the first failure.
#   scripts/check.sh [--quick] [--base REF]
#   --quick     skip the sanitizer build
#   --base REF  lint port/ on the lines changed since REF (default HEAD: the uncommitted ones)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
if [ -z "${IN_NIX_SHELL:-}" ]; then
    exec nix develop -c "$0" "$@"
fi

quick=0
base=HEAD
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) quick=1 ;;
        --base) base="$2"; shift ;;
        *) echo "usage: scripts/check.sh [--quick] [--base REF]" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n== %s\n' "$1"; }

step "build"
cmake --preset linux >/dev/null
cmake --build --preset linux
step "tests"
ctest --preset linux
step "lint"
python3 scripts/lint.py --base "$base"
step "smoke"
if [ -d data/en ]; then
    python3 scripts/smoke.py build/linux/verdite1
    python3 scripts/smoke.py build/linux/verdite1 --renderer vulkan
else
    echo "skipped: no data/en"
fi

if [ "$quick" = 1 ]; then
    printf '\ncheck: passed (quick: no sanitizer build)\n'
    exit 0
fi
step "sanitize build"
cmake --preset sanitize >/dev/null
cmake --build --preset sanitize
step "sanitize tests"
ctest --preset sanitize
step "sanitize smoke"
if [ -d data/en ]; then
    python3 scripts/smoke.py build/sanitize/verdite1
    python3 scripts/smoke.py build/sanitize/verdite1 --renderer vulkan
else
    echo "skipped: no data/en"
fi
printf '\ncheck: passed\n'
