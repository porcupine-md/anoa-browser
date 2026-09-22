# The class name must be the CamelCase form of the file base name
# (anoa-linux -> AnoaLinux): Homebrew derives it from the file name, and one
# mismatch makes the whole tap un-tappable. scripts/validate_homebrew_templates.sh
# enforces this before a tap commit is pushed (issue #37).
class AnoaLinux < Formula
  desc "Headless browser built on Qt6/QWebEngine with CDP support"
  homepage "https://github.com/porcupine-md/anoa-browser"
  version "{{VERSION}}"
  license "MIT"

  # Two bundles, picked by the machine doing the installing. Before this, an
  # arm64 Linux got the x86_64 tarball and a binary it could not exec.
  on_intel do
    url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-linux-x86_64.tar.gz"
    sha256 "{{LINUX_X86_64_SHA256}}"
  end

  on_arm do
    url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-linux-aarch64.tar.gz"
    sha256 "{{LINUX_AARCH64_SHA256}}"
  end

  def install
    libexec.install Dir["*"]
    # anoa.sh sets LD_LIBRARY_PATH / QtWebEngine paths relative to its
    # own (symlink-resolved) location, so a plain symlink is enough. The
    # terminal viewer is a subcommand of the same binary (`anoa
    # terminal`), so this is the only launcher the bundle needs.
    bin.install_symlink libexec/"anoa.sh" => "anoa"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/anoa --version")
  end
end
