#!/usr/bin/env bash
# usage: build-firmware.sh <ncs-workspace> <build-dir>, with BOARD set in the environment
set -euo pipefail

app=$(cd "$(dirname "$0")/.." && pwd)
case $2 in /*) build=$2 ;; *) build=$PWD/$2 ;; esac
# west finds Zephyr and the NCS modules from the workspace it runs in.
cd "$1"
west build --no-sysbuild -d "$build" "$app"
