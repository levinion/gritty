{
  description = "Gritty - Fast, CPU-rendered terminal emulator in Modern C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    esctest2-src = {
      url = "github:ThomasDickey/esctest2/2798f12149a19c3295e9b4853ab2da4b2eff1b2b";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }:
    let
      # macOS support is Apple Silicon only; Intel macOS is out of scope.
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs supportedSystems (system: f (import nixpkgs {
        inherit system;
        config = { };
      }));
      # macOS 26 is the minimum deployment target: libc++ gates
      # floating-point std::from_chars at macOS 26, and the pinned nixpkgs
      # provides the matching SDK 27 derivation.
      darwinSdk = pkgs: pkgs.apple-sdk_27 or
        (throw "gritty requires a nixpkgs providing apple-sdk_27");
      darwinDeploymentTarget = "26.0";
      # stdenv's Darwin preHook re-exports hostPlatform.darwinMinVersion, so
      # overriding only the derivation environment is not sufficient.
      stdenvFor = pkgs:
        if !pkgs.stdenv.hostPlatform.isDarwin then pkgs.stdenv else
        pkgs.stdenv.override {
          hostPlatform = pkgs.stdenv.hostPlatform // {
            darwinMinVersion = darwinDeploymentTarget;
            darwinSdkVersion = "27.0";
          };
        };
    in
    {
      devShells = forAllSystems (pkgs:
        let
          base = (pkgs.mkShell.override { stdenv = stdenvFor pkgs; }) {
            MACOSX_DEPLOYMENT_TARGET = darwinDeploymentTarget;
            nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.clang-tools
            pkgs.tmux # Real terminal compatibility regression tests
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland-scanner
          ];

          buildInputs = [
            pkgs.pixman
            pkgs.freetype
            pkgs.harfbuzz
            pkgs.utf8proc
            pkgs.tomlc17
            pkgs.doctest
            pkgs.libpng
            pkgs.zlib
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.libxkbcommon
            pkgs.fontconfig
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            (darwinSdk pkgs)
          ];
          };
          # External acceptance tools are opt-in; never linked into gritty and
          # never pulled into the ordinary development shell or package.
          esctest2 = pkgs.writeShellApplication {
            name = "esctest2";
            runtimeInputs = [ pkgs.python3 ];
            text = ''exec python3 ${self.inputs.esctest2-src}/esctest/esctest.py "$@"'';
          };
          # Keep the runner and corpus, not the package's gnuplot helpers.
          vtebench = pkgs.vtebench.overrideAttrs (_: {
            postInstall = ''
              mkdir -p "$out/share/vtebench"
              cp -r benchmarks "$out/share/vtebench/"
            '';
            postFixup = ''
              patchShebangs "$out/share/vtebench"
              wrapProgram "$out/bin/vtebench" \
                --add-flags "--benchmarks $out/share/vtebench/benchmarks"
            '';
          });
          typometerZip = pkgs.fetchurl {
            url = "https://github.com/pavelfatin/typometer/releases/download/v1.0.1/typometer-1.0.1-bin.zip";
            hash = "sha256-ivwXbve1uUAhZAvjCJx/etvpTwvcWxW4QwjnZfy2jTU=";
          };
          typometerJar = pkgs.runCommand "typometer-1.0.1-jar" { nativeBuildInputs = [ pkgs.unzip ]; } ''
            unzip -q ${typometerZip}
            mkdir -p "$out"
            cp typometer-1.0.1/typometer-1.0.1.jar "$out/typometer.jar"
            cp typometer-1.0.1/LICENSE "$out/LICENSE"
          '';
          available = package: pkgs.lib.meta.availableOn pkgs.stdenv.hostPlatform package;
          typometer = pkgs.writeShellApplication {
            name = "typometer";
            runtimeInputs = pkgs.lib.optionals (available pkgs.jdk) [ pkgs.jdk ];
            text = ''exec java -jar ${typometerJar}/typometer.jar "$@"'';
          };
          extraShell = packages: attrs: (pkgs.mkShell.override {
            stdenv = stdenvFor pkgs;
          }) ({
            inputsFrom = [ base ];
            packages = [ pkgs.python3 ] ++ packages;
          } // attrs);
        in {
          default = base;
          protocol = extraShell [ esctest2 ] {
            GRITTY_TEST_FONT = "${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSansMono.ttf";
          };
          visual = extraShell (pkgs.lib.optionals (available pkgs.notcurses) [ pkgs.notcurses ]) {};
          throughput = extraShell (pkgs.lib.optionals (available vtebench) [ vtebench ]
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.foot ]) {
            GRITTY_VTEBENCH_BENCHMARKS = "${vtebench}/share/vtebench/benchmarks";
          };
          latency = extraShell [ typometer ] {};
        });

      packages = forAllSystems (pkgs:
        let
          # Nix's generated Meson cross-file does not declare objcpp; merge a
          # minimal supplement so add_languages('objcpp') can use clang++.
          objcppCrossFile = pkgs.writeText "gritty-objcpp-cross-file.conf" ''
            [binaries]
            objcpp = 'clang++'
          '';
        in {
        default = (stdenvFor pkgs).mkDerivation {
          pname = "gritty";
          version = "0.1.0";
          src = ./.;
          MACOSX_DEPLOYMENT_TARGET = darwinDeploymentTarget;
          mesonFlags = pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            "--cross-file=${objcppCrossFile}"
          ];

          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.tmux
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland-scanner
          ];

          buildInputs = [
            pkgs.pixman
            pkgs.freetype
            pkgs.harfbuzz
            pkgs.utf8proc
            pkgs.tomlc17
            pkgs.doctest
            pkgs.libpng
            pkgs.zlib
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.libxkbcommon
            pkgs.fontconfig
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            (darwinSdk pkgs)
          ];
        };
      });
    };
}
