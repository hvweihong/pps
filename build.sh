#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCS_WORKSPACE="${NCS_WORKSPACE:-/home/hv/ncs}"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-${NCS_WORKSPACE}}"
ZEPHYR_BASE="${ZEPHYR_BASE:-${ZEPHYR_WORKSPACE}/zephyr}"
ZEPHYR_VENV="${ZEPHYR_VENV:-${ZEPHYR_WORKSPACE}/.venv}"
if [[ ! -x "${ZEPHYR_VENV}/bin/west" && -x "/home/hv/zephyrproject/.venv312/bin/west" ]]; then
	ZEPHYR_VENV="/home/hv/zephyrproject/.venv312"
fi
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/home/hv/zephyr-sdk-0.17.4}"
BOARD="${BOARD:-xiao_ble/nrf52840}"
BUILD_DIR="${BUILD_DIR:-}"
PRISTINE="always"
MODE="slave"
EXTRA_CONF=()

usage() {
	cat <<EOF
Usage: $(basename "$0") [master|slave] [options] [-- extra west build args]

Options:
  -b, --board BOARD        Zephyr board target. Default: ${BOARD}
  -d, --build-dir DIR      Build output directory. Default: build/<mode>
      --no-pristine        Reuse the existing build directory.
  -h, --help              Show this help.

Environment overrides:
  NCS_WORKSPACE            Default: ${NCS_WORKSPACE}
  ZEPHYR_WORKSPACE         Default: ${ZEPHYR_WORKSPACE}
  ZEPHYR_BASE              Default: ${ZEPHYR_BASE}
  ZEPHYR_VENV              Default: ${ZEPHYR_VENV}
  ZEPHYR_SDK_INSTALL_DIR   Default: ${ZEPHYR_SDK_INSTALL_DIR}
  BOARD                    Default: ${BOARD}
  BUILD_DIR                Default: ${BUILD_DIR}
EOF
}

if [[ $# -gt 0 ]]; then
	case "$1" in
		master|slave)
			MODE="$1"
			shift
			;;
	esac
fi

while [[ $# -gt 0 ]]; do
	case "$1" in
		-b|--board)
			BOARD="$2"
			shift 2
			;;
		-d|--build-dir)
			BUILD_DIR="$2"
			shift 2
			;;
		--no-pristine)
			PRISTINE="auto"
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		--)
			shift
			break
			;;
		*)
			break
			;;
	esac
done

WEST="${ZEPHYR_VENV}/bin/west"

if [[ ! -x "${WEST}" ]]; then
	echo "error: west not found at ${WEST}" >&2
	echo "Set ZEPHYR_VENV to the Zephyr Python virtual environment." >&2
	exit 1
fi

if [[ ! -d "${ZEPHYR_BASE}" ]]; then
	echo "error: ZEPHYR_BASE does not exist: ${ZEPHYR_BASE}" >&2
	echo "This app now builds against nRF Connect SDK because it uses MPSL." >&2
	echo "Set NCS_WORKSPACE or ZEPHYR_WORKSPACE to a synced NCS workspace." >&2
	exit 1
fi

if [[ ! -f "${ZEPHYR_WORKSPACE}/nrfxlib/mpsl/include/mpsl_timeslot.h" ]]; then
	echo "error: MPSL headers not found in ${ZEPHYR_WORKSPACE}/nrfxlib" >&2
	echo "Run: west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.1 ${NCS_WORKSPACE}" >&2
	echo "Then: (cd ${NCS_WORKSPACE} && west update)" >&2
	exit 1
fi

if [[ ! -d "${ZEPHYR_SDK_INSTALL_DIR}" ]]; then
	echo "error: ZEPHYR_SDK_INSTALL_DIR does not exist: ${ZEPHYR_SDK_INSTALL_DIR}" >&2
	exit 1
fi

export PATH="${ZEPHYR_VENV}/bin:${PATH}"
export ZEPHYR_BASE
export ZEPHYR_SDK_INSTALL_DIR

case "${MODE}" in
	master)
		BUILD_DIR="${BUILD_DIR:-${APP_DIR}/build/master}"
		EXTRA_CONF=(-DEXTRA_CONF_FILE="${APP_DIR}/master.conf")
		;;
	slave)
		BUILD_DIR="${BUILD_DIR:-${APP_DIR}/build/slave}"
		EXTRA_CONF=(-DEXTRA_CONF_FILE="${APP_DIR}/slave.conf")
		;;
esac

echo "Building ${APP_DIR}"
echo "Mode: ${MODE}"
echo "Board: ${BOARD}"
echo "Build dir: ${BUILD_DIR}"

exec "${WEST}" build \
	--no-sysbuild \
	-p "${PRISTINE}" \
	-b "${BOARD}" \
	"${APP_DIR}" \
	-d "${BUILD_DIR}" \
	"${EXTRA_CONF[@]}" \
	"$@"
