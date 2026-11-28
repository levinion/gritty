<p align="center">
  <img src="resources/gritty.png" alt="Gritty logo" width="128">
</p>

<h1 align="center">Gritty</h1>

<p align="center">
  A minimal, CPU-rendered C++23 terminal built for <code>tmux</code> on Apple Silicon macOS.
</p>

On Linux I am a daily [`foot`](https://codeberg.org/dnkl/foot) user — it is one of the best terminals ever written, and it proved to me that a well-engineered CPU terminal can be faster, lighter, and sharper than most hardware-accelerated ones. When I saw [`foot-macos`](https://github.com/gobijan/foot-macos), it gave me the idea to build Gritty: a native C++23 companion for `tmux` on Apple Silicon macOS, bringing `foot`-style crisp text and low latency without Metal/OpenGL shaders or CoreText's blurry font smoothing.

It uses CoreText only to find font files and fallbacks. Everything else — glyph rasterization (FreeType2), shaping (HarfBuzz), compositing (Pixman), and buffer management (`IOSurface`) — happens directly on the CPU.

Because `tmux` already handles panes, tabs, scrollback, search, copy mode, and reflow well, Gritty deliberately implements none of them. Right now only **Apple Silicon macOS (`arm64`)** has a window backend (Intel macOS is unsupported, and a Linux/Wayland backend has not been written yet).

## Installation

### Homebrew (macOS Apple Silicon)

Install `Gritty.app` and the `gritty` CLI binary from GitHub Releases via the in-repo cask ([Casks/gritty.rb](Casks/gritty.rb)):

```sh
brew tap levinion/gritty https://github.com/levinion/gritty
brew install --cask gritty
```

### From source (macOS, without Nix)

Requires Apple Silicon macOS with Xcode Command Line Tools (`clang++` with C++23 support) and [Homebrew](https://brew.sh) dependencies:

```sh
brew install meson ninja pkg-config pixman freetype harfbuzz utf8proc libpng
# Optional, if you want to build and run the test suite:
brew install doctest tmux
```

Clone, build, and launch:

```sh
git clone https://github.com/levinion/gritty.git && cd gritty
meson setup build/release --buildtype=release -Db_lto=true -Dtests=false
meson compile -C build/release gritty
./build/release/src/gritty
```

To bundle a signed local `Gritty.app` (`dist/Gritty.app`), run [packaging/macos/package.sh](packaging/macos/package.sh) after building.

### With Nix

Run directly from the flake without cloning:

```sh
nix run github:levinion/gritty
```

Or build locally after cloning (`nix build`, or using [tools/build.sh](tools/build.sh) which enters the Nix devShell and manages `build/release` and `build/debug` automatically):

```sh
git clone https://github.com/levinion/gritty.git && cd gritty
nix build
./result/bin/gritty
# or:
./tools/build.sh release build
./tools/build.sh release run
```

## Using with `tmux`

Gritty renders what modern `tmux` and Neovim expect out of the box: 24-bit color, styled and colored underlines (`Smulx` / `Setulc`), `DECSCUSR` cursor shapes, synchronized updates (`DECSET 2026`), bracketed paste, focus events, mouse reporting (`X10` / `SGR` / `SGR-Pixels`), `OSC 52` clipboard writes, `XTGETTCAP`, Kitty keyboard (`CSI u`), and Kitty graphics (`APC G`, including `tmux` DCS passthrough and `U=1` Unicode placeholders). Sixel is intentionally not implemented.

If you want passthrough images, clipboard sync, styled underlines, and Kitty keys inside `tmux`, add this to `~/.tmux.conf`:

```tmux
set -g allow-passthrough on
set -g set-clipboard on
set -g extended-keys on
set -as terminal-features ',xterm-256color:RGB:extkeys:usstyle'
```

*(Gritty defaults `TERM` to `xterm-256color` and answers `XTVERSION` / `XTGETTCAP` directly.)*

Optionally, launch Gritty directly into a named `tmux` session (attach if `main` exists, otherwise create it):

```sh
gritty -e tmux new -As main
```

## Configuration and CLI

Gritty looks for a config file at `--config-file` (or `-c`), then `$XDG_CONFIG_HOME/gritty/config.toml`, then `~/.config/gritty/config.toml`. With no config file, it defaults to `Menlo` at `11.25pt` (`72 DPI` on macOS, matching Alacritty's default sizing), an `800×600` centered window, opaque `#111111` background, full titlebar, and zero padding.

Here is a minimal `~/.config/gritty/config.toml` (see [examples/config.toml](examples/config.toml) for all keys):

```toml
[terminal]
osc52 = "OnlyCopy" # Disabled | OnlyCopy | OnlyPaste | CopyPaste

# Optional explicit shell:
# [terminal.shell]
# program = "/bin/zsh"
# args = ["-l"]

[font]
normal = { family = "Menlo" }
size = 11.25
dpi = 72              # 72 on macOS to match CoreText; 96 on Linux
antialias = "grayscale" # grayscale | subpixel | none

[window]
decorations = "Full" # Full | None | Transparent | Buttonless
padding = { x = 6, y = 4 }

[platform.macos]
font = { normal = { family = "JetBrainsMono Nerd Font" }, size = 12.0 }
```

Common CLI flags (`gritty --help` for the full list):

- `-e <cmd> [args...]` (or `--`) — run a command instead of your login shell
- `-f <family>`, `-s <pt>` — override font family and size
- `-o <key=value>` — override any config key (repeatable, e.g. `-o window.padding=8`)
- `--config-file <path>` (`-c`) — use a specific TOML file
- `--cols <n>`, `--rows <n>` — initial grid size (defaults to `800×600` logical points when unset / `0`)
- `--working-directory <dir>` (`--cwd`), `-T <title>`, `--class <id>`, `--hold`, `--standalone`

Gritty runs under a single Dock icon/instance across launches while keeping every window independent: `Cmd+N` or launching `gritty` / `open -n -a Gritty` opens a new independent window, `Cmd+W` closes the focused window, closing the last window exits the instance cleanly, `Cmd++` / `Cmd+-` / `Cmd+0` (and trackpad pinch) zoom font size per window, and `window.confirm_quit` (`"needed"` by default, `"always"`, or `"never"`) controls double-`Cmd+Q` quit confirmation.

## Benchmarks

Running [`alacritty/vtebench`](https://github.com/alacritty/vtebench) on Apple Silicon macOS (`160×48`, 2× Retina) with empty default configs on both terminals (`Menlo 11.25pt`; Gritty `14×27` px cells vs Alacritty `13×26` px cells):

| Workload | Gritty (ms) | Alacritty (ms) | Ratio |
|---|---:|---:|---:|
| `cursor_motion` | **60.0** | 63.5 | 0.94x |
| `dense_cells` | **52.0** | 88.0 | 0.59x |
| `light_cells` | **63.5** | 93.0 | 0.68x |
| `scrolling` | **159.0** | 222.0 | 0.72x |
| `scrolling_bottom_region` | **175.0** | 802.0 | 0.22x |
| `scrolling_bottom_small_region` | **173.0** | 954.0 | 0.18x |
| `scrolling_fullscreen` | **77.5** | 106.0 | 0.73x |
| `scrolling_top_region` | **175.0** | 480.0 | 0.36x |
| `scrolling_top_small_region` | **174.0** | 274.0 | 0.64x |
| `unicode` | 97.0 | **91.0** | 1.07x |

Median over 20 runs (`n=13`/`11` for Alacritty's slowest region cases). Reproduce with:

```sh
python3 benchmark/run.py --targets gritty,alacritty
```

A quick caveat on why the gap is so large, especially on partial scroll regions: Gritty carries zero scrollback history (delegated to `tmux`), rotates a `uint16_t` row index table instead of moving cells during `DECSTBM` scrolls, scans ASCII with ARM64 NEON, and runs a single-threaded `kqueue` loop with 2 ms frame coalescing instead of synchronizing a separate PTY/render thread with a 10k-line history ring. On heavy non-CJK `unicode` streams where Gritty falls back to `utf8proc` and FreeType glyph loads, Alacritty edges ahead.

## Development

Run the headless unit suite and real `tmux` PTY integration tests (requires `doctest` and `tmux`):

```sh
# Using the Nix devShell wrapper
./tools/build.sh debug test
./tools/build.sh release test

# Or directly with Meson (without Nix)
meson test -C build/release --print-errorlogs
```

Code layout:

```text
src/
├── config/          # TOML loader (tomlc17), AppConfig, CLI parser
├── core/            # TerminalSession, TerminalEngine, ScreenGrid, VtParser, Kitty graphics
├── input/           # Key/mouse mapping (xterm + Kitty CSI u + SGR mouse)
├── render/          # FreeType2/HarfBuzz FontEngine, BoxDrawingRenderer, Pixman CpuRenderer
├── platform/macos/  # Cocoa window, IOSurface pool, kqueue loop, CoreText finder, IME
└── util/            # Move-only RAII wrappers, base64, formatting
tests/               # doctest unit tests + real tmux integration suite
benchmark/           # alacritty/vtebench harness (benchmark/run.py)
```

## Note on AI

Gritty is a personal experimental project, and all code in this repository was written by AI. While it includes unit tests, real `tmux` integration tests, and benchmarks, please keep that context in mind before relying on it in production.

## Acknowledgments

- [`foot`](https://codeberg.org/dnkl/foot) and `fcft` — an extraordinary terminal that proved a CPU-rendered emulator can outperform most GPU-accelerated terminals in speed, footprint, and font clarity. Gritty owes its Pixman + FreeType/HarfBuzz pipeline, damage tracking, and scroll-blitting model directly to `foot`.
- [`foot-macos`](https://github.com/gobijan/foot-macos) — for sparking the idea behind this project in the first place and serving as an important reference for the macOS client and `IOSurface` presentation layer.
- [`alacritty`](https://github.com/alacritty/alacritty) and [`vtebench`](https://github.com/alacritty/vtebench) for the throughput benchmark suite and default sizing reference.
- [`kitty`](https://sw.kovidgoyal.net/kitty/) for the keyboard (`CSI u`) and graphics protocol specifications.
- Core libraries and tools: Pixman, FreeType, HarfBuzz, utf8proc, libpng, zlib, [`tomlc17`](https://github.com/cktan/tomlc17), and `doctest`.

## License

Gritty is licensed under the [GNU General Public License v3.0](LICENSE).
