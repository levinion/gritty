cask "gritty" do
  version "0.1.0"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"

  # Local dry-run URL:
  # url "file://#{Dir.home}/Downloads/gritty-#{version}-macos-arm64.tar.gz"
  url "https://github.com/levinion/gritty/releases/download/v#{version}/gritty-#{version}-macos-arm64.tar.gz"

  name "Gritty"
  desc "CPU-rendered terminal emulator for tmux on Apple Silicon macOS"
  homepage "https://github.com/levinion/gritty"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on arch: :arm64
  depends_on macos: ">= :sequoia"
  depends_on formula: [
    "pixman",
    "freetype",
    "harfbuzz",
    "utf8proc",
    "libpng",
  ]

  app "Gritty.app"
  binary "#{appdir}/Gritty.app/Contents/MacOS/gritty"

  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-cr", "#{appdir}/Gritty.app"],
                   sudo: false
  end

  zap trash: [
    "~/.config/gritty",
    "~/Library/Preferences/top.maruka.gritty.plist",
    "~/Library/Saved Application State/top.maruka.gritty.savedState",
  ]
end
