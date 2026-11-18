#!/usr/bin/env bash
# Stable Meson profiles; all builds run in the project's Nix devShell.
set -euo pipefail
cd "$(dirname "$0")/.."

usage() {
  echo "Usage: $0 [release|debug] [build|test|run|setup] [arguments...]"
  echo "Defaults: release build. setup accepts Meson options (e.g. -Dgui_tests=true)."
}
if [[ ${1:-} == -h || ${1:-} == --help ]]; then
  usage
  exit 0
fi
profile=${1:-release}
action=${2:-build}
case "$profile" in
  release) options=(--buildtype=release) ;;
  debug) options=(--buildtype=debug) ;;
  *) usage >&2; exit 2 ;;
esac
case "$action" in
  build|test|run|setup) ;;
  *) usage >&2; exit 2 ;;
esac
if [[ -z ${IN_NIX_SHELL:-} ]]; then
  exec ./tools/dev.sh bash ./tools/build.sh "$@"
fi
if (( $# > 0 )); then shift; fi
if (( $# > 0 )); then shift; fi
build_dir="build/$profile"
if [[ ! -f "$build_dir/meson-private/coredata.dat" ]]; then
  if [[ $action == setup ]]; then
    meson setup "$build_dir" "${options[@]}" "$@"
  else
    meson setup "$build_dir" "${options[@]}"
  fi
elif [[ $action == setup ]]; then
  meson setup --reconfigure "$build_dir" "${options[@]}" "$@"
fi
# clangd discovers the last selected profile without a hard-coded build path.
if [[ ! -e compile_commands.json || -L compile_commands.json ]]; then
  ln -sfn "$build_dir/compile_commands.json" compile_commands.json
fi
case "$action" in
  setup) ;;
  build) meson compile -C "$build_dir" "$@" ;;
  test) meson test -C "$build_dir" --print-errorlogs "$@" ;;
  run)
    meson compile -C "$build_dir" gritty
    exec "$build_dir/src/gritty" "$@"
    ;;
esac
