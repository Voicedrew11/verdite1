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
