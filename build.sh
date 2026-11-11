#!/usr/bin/env bash
set -euo pipefail

app=$(cd "$(dirname "$0")" && pwd)
ncs=/opt/ncs/v3.4.2
export PATH=/opt/ncs/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=/opt/ncs/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

cd "$ncs"
case ${1:-build} in
build)
	west build -b promicro_nrf52840/nrf52840/uf2 -d "$app/build/app" "$app"
	;;
test)
	cmake -S "$app/tests" -B "$app/build/tests" -G Ninja
	cmake --build "$app/build/tests"
	ctest --test-dir "$app/build/tests" --output-on-failure
	cargo test --manifest-path "$app/tool/Cargo.toml"
	cargo build --manifest-path "$app/tool/Cargo.toml"
	;;
flash)
	cargo run --manifest-path "$app/tool/Cargo.toml" -- ${2:+--serial "$2"} flash --yes \
		"$app/build/app/$(basename "$app")/zephyr/zephyr.uf2"
	;;
*)
	echo "usage: $0 build|test|flash [serial]" >&2
	exit 2
	;;
esac
