{
  description = "Gritty - Fast, CPU-rendered terminal emulator in Modern C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
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
          base = (pkgs.mkShell.override { stdenv = stdenvFor pkgs; }) ({
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
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            GRITTY_TEST_FONT = "${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSansMono.ttf";
          });
          # Keep the runner and corpus, not the package's gnuplot helpers.
          vtebench = pkgs.vtebench.overrideAttrs (_: {
            postInstall = ''
              mkdir -p "$out/share/vtebench"
              cp -r benchmarks "$out/share/vtebench/"
            '';
            postFixup = ''
              patchShebangs "$out/share/vtebench"
              mv "$out/bin/vtebench" "$out/bin/.vtebench-wrapped"
              {
                printf '#!%s -e\n' "${pkgs.runtimeShell}"
                cat <<'EOF'
has_benchmarks=0
for arg in "$@"; do
  case "$arg" in
    -b*|--benchmarks|--benchmarks=*)
      has_benchmarks=1
      break
      ;;
  esac
done
if [ "$has_benchmarks" -eq 1 ]; then
  exec -a "$0" "@out@/bin/.vtebench-wrapped" "$@"
else
  exec -a "$0" "@out@/bin/.vtebench-wrapped" --benchmarks "@out@/share/vtebench/benchmarks" "$@"
fi
EOF
              } | sed "s|@out@|$out|g" > "$out/bin/vtebench"
              chmod +x "$out/bin/vtebench"
            '';
          });
          available = package: pkgs.lib.meta.availableOn pkgs.stdenv.hostPlatform package;
        in {
          default = base;
          throughput = (pkgs.mkShell.override {
            stdenv = stdenvFor pkgs;
          }) {
            inputsFrom = [ base ];
            packages = [ pkgs.python3 ]
              ++ pkgs.lib.optionals (available vtebench) [ vtebench ]
              ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.foot ];
            GRITTY_VTEBENCH_BENCHMARKS = "${vtebench}/share/vtebench/benchmarks";
          };
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
          version = "2026.11.26";
          src = ./.;
          MACOSX_DEPLOYMENT_TARGET = darwinDeploymentTarget;
          mesonFlags = [
            "--buildtype=release"
            "-Db_lto=true"
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
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
