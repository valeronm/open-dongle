#!/bin/sh
# POSIX sh, because it runs piped from curl on a machine with nothing else set up.
# The tool is unsigned, and curl, unlike a browser, sets no quarantine flag for macOS to refuse.
set -eu

name="spare-dongle"
repo="valeronm/$name"
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
dir="$name-$version-$platform"
archive="$dir.tar.gz"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"

fetch_verified() {
  curl -fsSL -o "$tmp/$1" "$base/$1"
  awk -v f="$1" '$2 == f' "$tmp/SHA256SUMS" > "$tmp/$1.sha256"
  if [ ! -s "$tmp/$1.sha256" ]; then
    echo "get.sh: SHA256SUMS of $tag has no line for $1" >&2
    exit 1
  fi
  if ! (cd "$tmp" && verify_checksum "$1.sha256" >/dev/null); then
    echo "get.sh: $1 does not match SHA256SUMS of $tag" >&2
    exit 1
  fi
}

fetch_verified "$archive"
tar xzf "$tmp/$archive" -C "$tmp"

mkdir -p "$prefix"
install -m 0755 "$tmp/$dir/$name" "$prefix/$name"
echo "$name $version installed to $prefix/$name"

share="$HOME/.local/share/$name"
mkdir -p "$share"

# Piped from curl, the script's stdin is the script itself.
can_ask() { (: </dev/tty) 2>/dev/null; }
ask() {
  can_ask || return 1
  printf '%s [y/N] ' "$1" >/dev/tty
  read -r answer </dev/tty || answer=
  case "$answer" in y | Y | yes) ;; *) return 1 ;; esac
}

rules="70-$name.rules"
if [ -f "$tmp/$dir/$rules" ]; then
  install -m 0644 "$tmp/$dir/$rules" "$share/$rules"
  if ! cmp -s "$share/$rules" "/etc/udev/rules.d/$rules"; then
    if ask "Install the udev rule that lets $name reach the board without root, using sudo?" &&
      sudo cp "$share/$rules" /etc/udev/rules.d/ &&
      sudo udevadm control --reload &&
      sudo udevadm trigger --subsystem-match=hidraw --subsystem-match=tty; then
      echo "udev rule installed"
    else
      echo "To let $name reach the board without root, install its udev rule:"
      printf '  sudo cp "%s" /etc/udev/rules.d/\n' "$share/$rules"
      echo "  sudo udevadm control --reload"
      echo "  sudo udevadm trigger --subsystem-match=hidraw --subsystem-match=tty"
    fi
  fi
fi

# The firmware's file name carries the board's name.
firmware=$(awk '$2 ~ /\.uf2$/ { n++; f = $2 } END { if (n == 1) print f }' "$tmp/SHA256SUMS")
if [ -z "$firmware" ]; then
  echo "get.sh: SHA256SUMS of $tag lists no single firmware file to download" >&2
  exit 1
fi
fetch_verified "$firmware"
install -m 0644 "$tmp/$firmware" "$share/$firmware"
echo "firmware $version saved to $share/$firmware"

if can_ask && "$prefix/$name" flash "$share/$firmware" </dev/tty; then
  exit 0
fi
echo "To flash it onto a plugged-in dongle or board:"
printf '  "%s" flash "%s"\n' "$prefix/$name" "$share/$firmware"
