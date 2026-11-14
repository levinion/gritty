#!/usr/bin/env bash
set -e
if [ -e /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh ]; then
  . /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
fi
exec nix --extra-experimental-features "nix-command flakes" develop -c "$@"
