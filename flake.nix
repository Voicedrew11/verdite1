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
    in {
      devShells.${system}.default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
        packages = with pkgs; [ cmake ninja pkg-config clang python3 git sdl3 libGL libglvnd ];
        KF_ENGLISH_PATCH = "${englishDelta}";
      };
    };
}
