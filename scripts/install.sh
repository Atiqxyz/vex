#!/usr/bin/env bash
set -euo pipefail

repo="Atiqxyz/vex"
os="$(uname -s)"
arch="$(uname -m)"

case "$os:$arch" in
  Linux:x86_64|Linux:amd64)
    platform="linux-x64"
    archive="vex-linux-x64.tar.gz"
    checksum_tool="sha256sum"
    ;;
  Darwin:x86_64)
    platform="macos-x64"
    archive="vex-macos-x64.tar.gz"
    checksum_tool="shasum"
    ;;
  Darwin:arm64|Darwin:aarch64)
    platform="macos-arm64"
    archive="vex-macos-arm64.tar.gz"
    checksum_tool="shasum"
    ;;
  *)
    echo "No VEX release package is available for $os/$arch yet." >&2
    exit 1
    ;;
esac

base_url="${VEX_RELEASE_BASE_URL:-https://github.com/$repo/releases/latest/download}"
install_root="${VEX_INSTALL_DIR:-$HOME/.local/opt/vex}"
install_root="$(mkdir -p "$(dirname "$install_root")" && cd "$(dirname "$install_root")" && pwd)/$(basename "$install_root")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading VEX for $platform..."
curl --fail --location --retry 3 "$base_url/$archive" --output "$tmp/$archive"
curl --fail --location --retry 3 "$base_url/SHA256SUMS.txt" --output "$tmp/SHA256SUMS.txt"

expected="$(awk -v asset="$archive" '$2 == asset { print $1; exit }' "$tmp/SHA256SUMS.txt")"
if [[ -z "$expected" ]]; then
  echo "The release checksum file does not contain $archive" >&2
  exit 1
fi

if [[ "$checksum_tool" == "shasum" ]]; then
  actual="$(shasum -a 256 "$tmp/$archive" | awk '{print $1}')"
else
  actual="$(sha256sum "$tmp/$archive" | awk '{print $1}')"
fi
if [[ "$actual" != "$expected" ]]; then
  echo "VEX archive checksum mismatch. Expected $expected, got $actual" >&2
  exit 1
fi

tar -xzf "$tmp/$archive" -C "$tmp"
payload="$tmp/vex"
if [[ ! -x "$payload/bin/vex" || ! -x "$payload/toolchain/zig" ]]; then
  echo "The downloaded archive is missing the VEX executable or bundled Zig toolchain." >&2
  exit 1
fi

if [[ -e "$install_root" ]]; then
  if [[ ! -f "$install_root/.vex-managed" ]]; then
    echo "Refusing to replace $install_root because it is not marked as a VEX-managed installation." >&2
    exit 1
  fi
  rm -rf "$install_root"
fi
mkdir -p "$(dirname "$install_root")"
mv "$payload" "$install_root"
: > "$install_root/.vex-managed"

path_line='export PATH="$VEX_HOME/bin:$PATH"'
home_line="export VEX_HOME=\"$install_root\""
profile_files=("$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc")
if [[ "$os" == "Darwin" ]]; then
  profile_files+=("$HOME/.zprofile")
fi
for profile in "${profile_files[@]}"; do
  touch "$profile"
  if ! grep -Fqx "$home_line" "$profile"; then
    {
      printf '\n# VEX compiler\n'
      printf '%s\n' "$home_line"
      printf '%s\n' "$path_line"
    } >> "$profile"
  fi
done

export VEX_HOME="$install_root"
export PATH="$VEX_HOME/bin:$PATH"

vsix="$install_root/share/vex/vex-language.vsix"
if [[ -n "${VEX_SKIP_VSCODE:-}" ]]; then
  : # Installer integration tests intentionally skip editor modification.
elif command -v code >/dev/null 2>&1 && [[ -f "$vsix" ]]; then
  echo "Installing the VEX extension in Visual Studio Code..."
  code --install-extension "$vsix" --force || echo "VS Code extension installation failed; install it manually from: $vsix" >&2
elif [[ -f "$vsix" ]]; then
  echo "VS Code's 'code' command was not found. The extension is available at: $vsix"
fi

echo
echo "VEX is installed at $install_root"
echo "Open a new terminal or reload your shell configuration."
echo "Then run: vex --version"
