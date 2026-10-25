{
  description = "Verdite1: King's Field (SLPS-00017) on sushi-shi's decomp, development shell";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/64c08a7ca051951c8eae34e3e3cb1e202fe36786";
  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      # John Osborne's (weissvulf) English translation v1.0: the translator's own PPF release,
      # fetched from the Wayback Machine copy of their site, as kf1-enhanced's flake does. The delta
      # is derived locally and applied only to the player's own Japanese disc resources.
      englishTranslation = pkgs.fetchurl {
        name = "Kings_Field_Jap_to_Eng_v1.0.rar";
        url = "https://web.archive.org/web/20211128030012id_/https://www.angelfire.com/art3/weissvulf/Kings_Field_Jap_to_Eng_v1.0.rar";
        hash = "sha256-1BMeSi4+nzwBiOTCxsLspjLcWpVNqNtK8pm8T74u224=";
      };
      # The scripts are the decomp's (decomp/scripts), which a flake cannot see in a submodule, so
      # the derivation reads them from the pinned fork.
      decompScripts = pkgs.fetchFromGitHub {
        owner = "Voicedrew11";
        repo = "kings-field-decomp";
        rev = "7bd4567cf3ac285244d724bfb9945d92ad7abe4e";
        hash = "sha256-vUH4dUyahAl9MRQoudWAAWv2f2jUJ8rtrJR2KSCpNgQ=";
      };
      englishDelta = pkgs.runCommand "kings-field-english-v1.kfdelta" {
        nativeBuildInputs = [ pkgs.libarchive pkgs.python3 ];
      } ''
        bsdtar -xOf ${englishTranslation} "KF Jap to Eng v1.0.ppf" > translation.ppf
        python3 ${decompScripts}/scripts/english_patch.py from-ppf --ppf translation.ppf \
          --layout ${decompScripts}/scripts/slps-00017-layout.tsv --output "$out"
      '';
      # The Android SDK and NDK (docs/ANDROID_PLAN.md). Google's SDK licence was accepted by the
      # user on 2026-10-25; it is accepted here, for the android shell only.
      androidPkgs = import nixpkgs {
        inherit system;
        config = { android_sdk.accept_license = true; allowUnfree = true; };
      };
      # The NDK and SDK levels SDL3's own android-project uses.
      androidNdkVersion = "28.2.13676358";
      # withEmulator adds the emulator and an Android 15 x86_64 image (the android-x86_64 preset).
      androidSdk = withEmulator: (androidPkgs.androidenv.composeAndroidPackages {
        platformVersions = [ "35" ];
        buildToolsVersions = [ "35.0.0" ];
        includeNDK = true;
        ndkVersions = [ androidNdkVersion ];
        includeEmulator = withEmulator;
        includeSystemImages = withEmulator;
        systemImageTypes = [ "default" ];
        abiVersions = [ "x86_64" ];
      }).androidsdk;
      androidShell = withEmulator: let androidHome = "${androidSdk withEmulator}/libexec/android-sdk"; in pkgs.mkShell {
        packages = with pkgs; [ cmake ninja python3 git glslang gradle jdk17 android-tools libarchive ];
        ANDROID_HOME = androidHome;
        ANDROID_SDK_ROOT = androidHome;
        ANDROID_NDK_ROOT = "${androidHome}/ndk/${androidNdkVersion}";
        JAVA_HOME = "${pkgs.jdk17}";
        KF_ANDROID_NDK_VERSION = androidNdkVersion;
        KF_SDL3_SOURCE = "${pkgs.sdl3.src}";
        KF_VULKAN_HEADERS_SOURCE = "${pkgs.vulkan-headers.src}";
        KF_VOLK_SOURCE = "${pkgs.vulkan-volk.src}";
        KF_VMA_SOURCE = "${pkgs.vulkan-memory-allocator.src}";
        KF_VK_BOOTSTRAP_SOURCE = "${pkgs.vk-bootstrap.src}";
        KF_CPPTRACE_SOURCE = "${pkgs.cpptrace.src}";
        KF_ENGLISH_PATCH = "${englishDelta}";
      };
    in {
      # nix develop .#android: builds the arm64 libmain.so (cmake --preset android-arm64) and the
      # APK (android/). The libraries the desktop shell links are built from these same sources.
      devShells.${system} = {
      android = androidShell false;
      # nix develop .#android-emulator: the same, with the emulator (scripts/android-emulator.sh).
      android-emulator = androidShell true;
      default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
        # clang-tools first: its clang-tidy is wrapped to find the standard headers
        packages = with pkgs; [ clang-tools cmake ninja pkg-config clang python3 git sdl3 doctest cpptrace
          vulkan-headers vulkan-memory-allocator vulkan-volk vk-bootstrap glslang vulkan-validation-layers
          libarchive ]; # bsdtar opens the English patch's .rar
        KF_ENGLISH_PATCH = "${englishDelta}";
        # The game loads the host's Vulkan loader and drivers (run-linux.sh); the layers come from here.
        VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
      };
      };
    };
}
