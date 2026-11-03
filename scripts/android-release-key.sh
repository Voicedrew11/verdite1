#!/usr/bin/env bash
# Run once, ever: makes Verdite1's Android signing key outside the repository and gives it to GitHub's
# release workflow. A phone only accepts an update signed with the key it was first installed with,
# so losing this key means every player reinstalls and loses their app data: back the file up.
#   scripts/android-release-key.sh [KEYSTORE]   (default ~/.config/verdite1/android-release.jks)
set -euo pipefail
store="${1:-$HOME/.config/verdite1/android-release.jks}"
[ ! -e "$store" ] || { echo "$store already exists; it is the key. Not touching it." >&2; exit 1; }
mkdir -p "$(dirname "$store")"
read -rsp "A password for the key (keep it with the backup): " password
echo
[ "${#password}" -ge 8 ] || { echo "at least 8 characters" >&2; exit 1; }
keytool -genkeypair -keystore "$store" -storetype PKCS12 -storepass "$password" -alias verdite1 \
  -keyalg RSA -keysize 4096 -validity 36500 -dname "CN=Verdite1"
chmod 600 "$store"
base64 -w0 "$store" | gh secret set ANDROID_KEYSTORE_BASE64 -R Voicedrew11/verdite1
printf '%s' "$password" | gh secret set ANDROID_KEYSTORE_PASSWORD -R Voicedrew11/verdite1
echo "The key is $store and GitHub has it. Back up that file and the password now."
