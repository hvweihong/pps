#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USER_HOME="${HOME:?HOME is not set}"
NCS_WORKSPACE="${NCS_WORKSPACE:-${USER_HOME}/ncs}"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-${NCS_WORKSPACE}}"
ZEPHYR_BASE="${ZEPHYR_BASE:-${ZEPHYR_WORKSPACE}/zephyr}"
ZEPHYR_VENV="${ZEPHYR_VENV:-${ZEPHYR_WORKSPACE}/.venv}"
if [[ ! -x "${ZEPHYR_VENV}/bin/west" && -x "${USER_HOME}/zephyrproject/.venv312/bin/west" ]]; then
	ZEPHYR_VENV="${USER_HOME}/zephyrproject/.venv312"
fi
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-${USER_HOME}/zephyr-sdk-0.17.4}"
BOARD="${BOARD:-xiao_ble/nrf52840}"
BUILD_DIR="${BUILD_DIR:-}"
PRISTINE="always"

usage() {
	cat <<EOF
Usage: $(basename "$0") [options] [-- extra west build args]

Options:
  -b, --board BOARD        Zephyr board target. Default: ${BOARD}
  -d, --build-dir DIR      Build output directory. Default: build/
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

Note: This firmware builds a unified image. Role (master/slave) is configured
      at runtime via NVS and CDC shell command 'param set role_id 0|1|2|3'.
EOF
}

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
			echo "error: unexpected argument: $1" >&2
			usage >&2
			exit 2
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
	echo "This app builds against nRF Connect SDK because it uses Nordic ESB and ECB." >&2
	echo "Set NCS_WORKSPACE or ZEPHYR_WORKSPACE to a synced NCS workspace." >&2
	exit 1
fi

if [[ ! -f "${ZEPHYR_WORKSPACE}/nrf/include/esb.h" ]]; then
	echo "error: ESB headers not found in ${ZEPHYR_WORKSPACE}/nrf/include" >&2
	echo "Run: west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.1 ${NCS_WORKSPACE}" >&2
	echo "Then: (cd ${NCS_WORKSPACE} && west update)" >&2
	exit 1
fi

if [[ ! -f "${ZEPHYR_BASE}/drivers/crypto/crypto_nrf_ecb.c" ]]; then
	echo "error: nRF ECB driver not found in ${ZEPHYR_BASE}/drivers/crypto" >&2
	exit 1
fi

if [[ ! -d "${ZEPHYR_SDK_INSTALL_DIR}" ]]; then
	echo "error: ZEPHYR_SDK_INSTALL_DIR does not exist: ${ZEPHYR_SDK_INSTALL_DIR}" >&2
	exit 1
fi

export PATH="${ZEPHYR_VENV}/bin:${PATH}"
export ZEPHYR_BASE
export ZEPHYR_SDK_INSTALL_DIR

BUILD_DIR="${BUILD_DIR:-${APP_DIR}/build}"

echo "Building ${APP_DIR}"
echo "Board: ${BOARD}"
echo "Build dir: ${BUILD_DIR}"

"${WEST}" build \
	--no-sysbuild \
	-p "${PRISTINE}" \
	-b "${BOARD}" \
	"${APP_DIR}" \
	-d "${BUILD_DIR}" \
	"$@"

ELF="${BUILD_DIR}/zephyr/zephyr.elf"
HEX="${BUILD_DIR}/zephyr/zephyr.hex"
UF2="${BUILD_DIR}/zephyr/zephyr.uf2"
[[ -f "${ELF}" && -f "${HEX}" && -f "${UF2}" ]] || {
	echo "error: expected build artifacts are missing" >&2
	exit 1
}
echo "ELF: ${ELF}"
echo "HEX: ${HEX}"
echo "UF2: ${UF2}"
