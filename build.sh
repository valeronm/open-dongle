#!/usr/bin/env bash
set -euo pipefail

app=$(cd "$(dirname "$0")" && pwd)
ncs=/opt/ncs/v3.4.2
nrfutil=/opt/ncs/nrfutil-venv/bin/adafruit-nrfutil
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
	;;
flash)
	port=${2:-/dev/ttyACM0}
	"$nrfutil" dfu genpkg --dev-type 0x0052 \
		--application "$app/build/app/nuphy-dongle/zephyr/zephyr.hex" "$app/build/app.zip"
	"$nrfutil" dfu serial --package "$app/build/app.zip" -p "$port" -b 115200 \
		--singlebank --touch 1200
	;;
*)
	echo "usage: $0 build|test|flash [port]" >&2
	exit 2
	;;
esac
