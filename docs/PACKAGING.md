# Packaging

How the port becomes something a person downloads, installs and hears about updates for. It does
what Verdite2 and Verdite3 do for a player; how it gets there differs because this port is compiled.

## What differs from the C# ports

Verdite2 and Verdite3 ship a launcher that recompiles the game on the player's machine, because their
assembly is a translation of FromSoftware's code baked to one disc. Here the game is the decomp's
source, so the package holds the compiled game, and there is no launcher and no first-run build. The
line about game data stays where it was: **no package holds any.** The game reads the player's own
disc at first launch (`verdite-port.md`), and the English translation is the translator's own patch,
downloaded then; `-DKF_ENGLISH_PATCH=/nonexistent` keeps the delta out of every package.

Co-op and the messages are in every package (`KF_ONLINE`, libdatachannel built from source with
OpenSSL linked in). On Android, OpenSSL is built while configuring (`cmake/android_openssl.cmake`),
once per build folder, and a phone joins rooms but does not host them ([COOP_PLAN.md](COOP_PLAN.md)).

## The packages

| what | made by | where it runs |
|---|---|---|
| `Verdite1-V-x86_64.AppImage` | `scripts/package-linux.sh` | podman or docker, `packaging/appimage/` |
| `Verdite1-V-win-x64.zip` | `scripts/package-windows.sh` | podman or docker, `packaging/windows/`: llvm-mingw, cross |
| `Verdite1-V-win-x64-setup.exe` | `packaging/windows/make-installer.ps1` | Windows, Inno Setup, from the zip |
| `Verdite1-V-android-arm64.apk` | `scripts/package-android.sh` | the android nix shell |

- **The AppImage** runs on glibc 2.35 or later and carries `libSDL3.so.0`, the C++ runtime, OpenSSL
  linked in, and bsdtar on `PATH` for the English patch's `.rar` (GNU tar cannot read it). Vulkan's
  loader and drivers are the host's. That OpenSSL looks for root certificates only where the build
  image's Debian keeps them, so `port/ext/online_linux_roots.cpp` names the file each distribution
  keeps; without it, online play fails on Fedora with "the server cannot be reached". The repository is mounted at its own path with git's common
  directory beside it, because a worktree's `.git` files name them absolutely. When a player runs it,
  it writes `~/.local/share/applications/verdite1.desktop` and the icon, so it is in the menu, naming
  the file it runs from; it rewrites that entry when the file has moved, and leaves an entry it did
  not write. `KF_ICON_INSTALL=0` turns that off.
- **The installer** is per user, in `%LOCALAPPDATA%\Programs\Verdite1`, and needs no administrator.
  Its `AppId` in `verdite1.iss` is how Windows knows an upgrade: never change it or give it to another
  port. An upgrade clears `mods\` and `licenses\` first, and keeps the game's `data\` beside it. An
  uninstall leaves `data\` and everything in the user folder, saves above all.
- **The English patch on Windows** is unpacked inside the game by libarchive, as on Android, not by
  Windows' `tar`: a player's machine reported "Cannot open that archive" after the download had passed
  its checksum (2026-11-04), so `tar` was the step that failed. The image carries libarchive's source
  and a static zlib for `.zip`. Without LZMA a `.7z` is refused on Windows, with a message to extract
  the `.ppf` and choose that.
- **The Windows build is a cross-compile** so it is built, and can be tried under wine, on the same
  machine as the rest. `scripts/package-windows.ps1` is gone with the native build. A disc image
  extracted by Windows has `E0` where Linux has `E0.`, so a Linux `data/` fails its check there:
  extract with the `.exe` first (`--disc IMAGE --extract-to DIR --extract-only --language ja`).
- **The APK** holds arm64 only and is built in `build/android-release`, so it takes nothing from a
  development build. A phone installs an update only signed with the same key and with a higher
  `versionCode` (MAJOR·10000 + MINOR·100 + PATCH). The key is made once, by
  `scripts/android-release-key.sh`, kept outside the repository and handed to GitHub as secrets.
  **Losing it means every player reinstalls and loses their app data; back it up.** A debug build
  has another key, so going between one and the other needs an uninstall too.

## Local macOS build

The macOS preset builds the C/C++ port natively for Apple Silicon. SDL3 owns the
window, input and audio; the Vulkan renderer uses bundled MoltenVK to draw through
Metal. This local build targets macOS 27 or later to match the Homebrew libraries
used to build it. Earlier macOS releases and Intel Macs have not been validated.
Crash-report symbolication and recovery still need macOS support: five existing
crash-report tests fail on this platform.

Install Xcode command-line tools and the build dependencies, then build and launch:

```sh
brew install cmake ninja sdl3 vulkan-headers vulkan-volk glslang molten-vk
git submodule update --init decomp
./script/build_and_run.sh
```

The script creates `dist/Verdite1.app`, bundles SDL3 and MoltenVK, signs it locally,
and opens it through Launch Services. `--build-only` only builds and packages;
`--verify` also checks that the launched process stays alive.
This is an ad-hoc signed local build, not a notarized release.
Online play is not included in this build configuration.

Game data, settings and saves live under
`~/Library/Application Support/Verdite/Verdite1/`, outside the app. To import an
already English-patched v1.0 BIN/CUE pair without changing the original files:

```sh
mkdir -p "$HOME/Library/Application Support/Verdite/Verdite1/data/en"
dist/Verdite1.app/Contents/MacOS/verdite1 \
  --disc "/path/to/King's Field (Japan) [English Patched v1.0].cue" \
  --language en \
  --extract-to "$HOME/Library/Application Support/Verdite/Verdite1/data/en" \
  --extract-only
```

Keep the CUE and its named BIN together for import. Extracted data is verified
against the game's known digest; neither disc data nor the translation is bundled
in the app. Launching the app normally then finds the imported English data.

## Local iOS device build

The iOS target builds SDL3, the game and its dependencies for arm64, and embeds
MoltenVK's signed iOS framework. It requires iOS 17 or newer. CMake generates an
Xcode project in `build/ios/kings_field.xcodeproj`; do not edit that generated project.
The asset catalog includes an opaque 1024px version of the repository's sword icon.
Regenerate it with:

```sh
swift scripts/render_apple_icon.swift packaging/appimage/verdite1.svg \
  ios/Assets.xcassets/AppIcon.appiconset/AppIcon.png
```

```sh
# Build without signing, for compile verification:
./script/build_ios.sh build

# Build with your Apple development team, install and launch on a paired iPhone:
KF_IOS_TEAM=YOUR_TEAM ./script/build_ios.sh install YOUR_DEVICE_ID
```

Xcode must have the team's signing identity and account configured, and the phone
must be unlocked with Developer Mode enabled. The script enables Xcode's automatic
provisioning. The default bundle ID is `io.github.voicedrew11.verdite1`; set
`KF_IOS_BUNDLE_ID` to use your own registered identifier. The override persists in
the ignored CMake build cache. Keep the same identifier when updating an installed
app to preserve its saves. A development build can only be installed on devices
included in its provisioning profile. Simulator builds are not supported yet.

The app uses landscape orientation and sizes its touch layout from SDL's current
safe area on every frame. The left stick walks and strafes; dragging the right side
looks around. Attack, Magic, Examine, Menu and the settings gear are separate touch
targets. Save and Load provide quick saves without a keyboard. Button haptics can
be disabled in settings. Connected gamepads can also
take over, hiding the touch overlay until the next screen touch.
On compact landscape windows (under 600 points tall), the left control inset is
capped at 16 points for thumb reach. Vertical and right-side insets are preserved;
the larger unfolded/tablet layout continues using the full safe area. Drawing uses
window points, while touch hit testing uses framebuffer pixels, including Retina scaling.

On first launch, import a BIN/ISO/CHD through Files. For a CUE, select both the CUE and
its named BIN together. This local build accepts the already English-patched v1.0
disc and does not download translation patches. The files are copied into the app,
verified and extracted, and the copies are removed once the game files are out of them.
Original files are kept. Documents is exposed in Files and
Finder file sharing, with extracted game data under `data/en` and saves beside it.

To transfer an already verified English data folder while installing, set
`KF_IOS_ENGLISH_DATA` to the folder containing `GAME.EXE`. That folder is copied into
the app's Documents/data/en; no game data is included in the built application.
Do not delete the app without backing up Documents if you want to keep saves.
Online multiplayer is not enabled in this build.

If command-line provisioning reports "No Accounts" even though Xcode shows a
signed-in account, open the generated project in Xcode, select scheme `verdite1`
and the connected device, then build once. Xcode's account window can also refresh
manual profiles. The resulting cached profile must include the target device;
a successful generic-device signature alone does not prove installability.

## The tag is the version

As Verdite3: there is no version file, and `scripts/version.sh` is the one rule. `VERDITE_VERSION` if
set, else the newest `vMAJOR.MINOR.PATCH` tag `HEAD` contains, else `0.0.0`. CMake and Gradle both ask
it. `verdite1 --version` prints it, and the About window and the log add the commit. A tagless or
shallow clone is `0.0.0`. The old `v0.1.0-pre.N` tags and `preview` are not versions.

## Cutting a release

```bash
scripts/release.sh 0.1.0     # tags HEAD; commits nothing, pushes nothing, and refuses a lower number
git push origin HEAD && git push origin v0.1.0
```

The tag starts `.github/workflows/release.yml`, which builds all four packages and the Android symbols and opens a **draft**
release. Publishing the draft is what players see: GitHub's `releases/latest` skips drafts and
prereleases, so nothing is announced until then.

A fifth file, `Verdite1-V-android-arm64-symbols.zip`, is the APK's `libmain.so` before it was
stripped: nothing for players, and what names a phone's crash report (`CRASH_REPORTS.md`). A preview's
stays with the workflow run, as an artifact.

## Previews

A build handed to testers: a commit, not a version. When one proves itself, the tag goes on that
commit. There is one at a time, the rolling `preview` prerelease, replaced whole:

```bash
git push origin main
scripts/prerelease.sh        # builds HEAD as GitHub has it
```

Each package is zipped with the `PREVIEW_PASSWORD` secret (AES-256, so 7-Zip or WinRAR opens it), the
asset names never change, and `preview-cleanup.yml` deletes it when a full release is published.
`gh workflow run` needs `release.yml` on the default branch.

## Secrets

| secret | for | set by |
|---|---|---|
| `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD` | every release and preview | `scripts/android-release-key.sh` |
| `PREVIEW_PASSWORD` | previews | `gh secret set PREVIEW_PASSWORD` |

## Telling the player about a new release

Notify only, as the C# ports: nothing is downloaded. `verdite/update_check.h` has the logic and
`port/ext/update_notice.cpp` the showing.

- **Asked** once a launch on its own thread, in a build for players (`-DKF_RELEASE=ON`, which all the
  packages set), through the same download as the English patch (curl on the desktop, Java on a
  phone). The answer of `api.github.com/repos/Voicedrew11/verdite1/releases/latest` is kept in
  `update.json` in the user folder, and the network is asked at most once an hour, as in Verdite2 and
  Verdite3: at once a day, a release went unannounced for up to a day. Offline or rate-limited, the
  kept answer stands.
- **Shown** once a launch, when the title menu is up and nothing else covers the game, never in play.
  A desktop draws a window (Download page, Skip this version, Later) that holds the game while it is
  open, and a gold "Update available!" badge in the F1 menu bar. Android shows a dialog with the same
  three buttons; a phone has no menu bar, so the setting's row in Interface says so too.
- **Skip** stops announcing that one tag; a later one is announced.
- **Off:** Interface > "Check for updates at launch". `KF_UPDATE_CHECK=0` turns it off for a run and
  `force` asks now, in any build. `KF_TEST_UPDATE_URL` asks another URL (a `file://` works on the
  desktop), which is how to see it without a release.

## CI

`ci.yml`, on every push: no disc data tracked, the game builds, the libraries' tests pass, and the
build carries the version. It has no disc, so it boots nothing; that stays `scripts/check.sh`'s, before a
push. `release.yml` also checks each package names the right version, and installs and uninstalls the
setup on a real Windows runner.

A run of `release.yml` was about ten minutes (2026-11-04), half of it building the same things again
on a fresh runner, so it keeps three things between runs:

- **The builder images** go to `ghcr.io/<owner>/verdite1-<name>-builder:<hash>` (`scripts/builder-image.sh`), the hash being
  the Containerfile's and the files it copies. A run pulls the image and builds it only when that
  recipe changed. Unchanged, a Containerfile never takes newer Ubuntu packages; edit it to move them.
  Locally the image is built as before, with podman's or docker's own layer cache.
- **ccache**, in all three builds. A package script keeps it in `build/ccache/<name>` (`KF_CCACHE_DIR`
  moves the desktop ones), and CI keeps it through `actions/cache`, at most 500 MB each.
  The repository is at the same path on every runner, so hits survive between runs; each build prints
  its hit rate after compiling.
- **Android's OpenSSL**, keyed on `cmake/android_openssl.cmake` and the flake.

What is left is the android nix shell's download (about three minutes) and Gradle.
