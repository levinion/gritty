#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-2026.11.26}"
BIN_PATH="${2:-build/release/src/gritty}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$BIN_PATH" != /* ]]; then
    BIN_PATH="$ROOT/$BIN_PATH"
fi

if [[ ! -f "$BIN_PATH" ]]; then
    echo "error: binary not found: $BIN_PATH" >&2
    exit 1
fi
if [[ ! -f "$ROOT/resources/gritty.icns" ]]; then
    echo "error: icon not found: $ROOT/resources/gritty.icns" >&2
    exit 1
fi

APP_DIR="$ROOT/dist/Gritty.app"
ARCHIVE="$ROOT/dist/gritty-${VERSION}-macos-arm64.tar.gz"

rm -rf "$ROOT/dist/Gritty.app" "$ARCHIVE" "$ARCHIVE.sha256"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"

cp "$BIN_PATH" "$APP_DIR/Contents/MacOS/gritty"
chmod +x "$APP_DIR/Contents/MacOS/gritty"
cp "$ROOT/resources/gritty.icns" "$APP_DIR/Contents/Resources/gritty.icns"
sed "s/@VERSION@/${VERSION}/g" "$ROOT/packaging/macos/Info.plist.in" > "$APP_DIR/Contents/Info.plist"

codesign --force --deep -s - "$APP_DIR"

tar -czf "$ARCHIVE" -C "$ROOT/dist" Gritty.app
shasum -a 256 "$ARCHIVE" | tee "$ARCHIVE.sha256"
