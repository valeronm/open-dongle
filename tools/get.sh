#!/bin/sh
# POSIX sh, because it runs piped from curl on a machine with nothing else set up.
# The tool is unsigned, and curl, unlike a browser, sets no quarantine flag for macOS to refuse.
set -eu

repo="valeronm/open-dongle"
prefix="${PREFIX:-$HOME/.local/bin}"

command -v curl >/dev/null 2>&1 || { echo "get.sh: curl is required" >&2; exit 1; }

# Linux ships coreutils' sha256sum and often no shasum, macOS the reverse.
if command -v sha256sum >/dev/null 2>&1; then
  verify_checksum() { sha256sum -c "$1"; }
elif command -v shasum >/dev/null 2>&1; then
  verify_checksum() { shasum -a 256 -c "$1"; }
else
  echo "get.sh: sha256sum or shasum is required" >&2
  exit 1
fi

case "$(uname -sm)" in
  "Linux x86_64") platform="x86_64-linux" ;;
  "Darwin arm64") platform="aarch64-macos" ;;
  *)
    echo "get.sh: no release for $(uname -sm)" >&2
    exit 1
    ;;
esac

# The API is rate-limited without a token; the latest-release redirect is not.
tag=$(curl -fsSIL -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest" | sed 's#.*/##')
case "$tag" in
  v[0-9]*) ;;
  *)
    echo "get.sh: could not resolve the latest release" >&2
    exit 1
    ;;
esac
version="${tag#v}"
base="https://github.com/$repo/releases/download/$tag"
dir="open-dongle-$version-$platform"
archive="$dir.tar.gz"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$archive" "$base/$archive"
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"
awk -v f="$archive" '$2 == f' "$tmp/SHA256SUMS" > "$tmp/$archive.sha256"
if [ ! -s "$tmp/$archive.sha256" ]; then
  echo "get.sh: SHA256SUMS of $tag has no line for $archive" >&2
  exit 1
fi
if ! (cd "$tmp" && verify_checksum "$archive.sha256" >/dev/null); then
  echo "get.sh: $archive does not match SHA256SUMS of $tag" >&2
  exit 1
fi
tar xzf "$tmp/$archive" -C "$tmp"

mkdir -p "$prefix"
install -m 0755 "$tmp/$dir/open-dongle" "$prefix/open-dongle"
echo "open-dongle $version installed to $prefix/open-dongle"

rules="70-open-dongle.rules"
if [ -f "$tmp/$dir/$rules" ]; then
  share="$HOME/.local/share/open-dongle"
  mkdir -p "$share"
  install -m 0644 "$tmp/$dir/$rules" "$share/$rules"
  if ! cmp -s "$share/$rules" "/etc/udev/rules.d/$rules"; then
    echo "To let open-dongle reach the board without root, install its udev rule and replug the board:"
    printf '  sudo cp "%s" /etc/udev/rules.d/\n' "$share/$rules"
    echo "  sudo udevadm control --reload"
  fi
fi
