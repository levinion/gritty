{
  description = "Squirt - Fast, CPU-rendered terminal emulator in Modern C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs supportedSystems (system: f (import nixpkgs {
        inherit system;
        config = { };
      }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.clang-tools
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland-scanner
          ];

          buildInputs = [
            pkgs.pixman
            pkgs.freetype
            pkgs.harfbuzz
            pkgs.utf8proc
            pkgs.tomlplusplus
            pkgs.doctest
            pkgs.libpng
            pkgs.zlib
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.libxkbcommon
            pkgs.fontconfig
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            (pkgs.apple-sdk or pkgs.apple-sdk_14 or pkgs.apple-sdk_15)
          ];
        };
      });

      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "squirt";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland-scanner
          ];

          buildInputs = [
            pkgs.pixman
            pkgs.freetype
            pkgs.harfbuzz
            pkgs.utf8proc
            pkgs.tomlplusplus
            pkgs.doctest
            pkgs.libpng
            pkgs.zlib
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.libxkbcommon
            pkgs.fontconfig
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            (pkgs.apple-sdk or pkgs.apple-sdk_14 or pkgs.apple-sdk_15)
          ];
        };
      });
    };
}
