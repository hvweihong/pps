#!/usr/bin/env bash
set -Eeuo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NCS_VERSION="${NCS_VERSION:-v3.3.1}"
NCS_REPOSITORY="${NCS_REPOSITORY:-https://github.com/nrfconnect/sdk-nrf}"
NCS_WORKSPACE="${NCS_WORKSPACE:-${HOME}/ncs}"
ZEPHYR_VENV="${ZEPHYR_VENV:-${NCS_WORKSPACE}/.venv}"
PYTHON_VERSION="${PYTHON_VERSION:-3.12}"
WEST_VERSION="${WEST_VERSION:-1.5.0}"

ZEPHYR_SDK_VERSION="${ZEPHYR_SDK_VERSION:-0.17.4}"
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-${HOME}/zephyr-sdk-${ZEPHYR_SDK_VERSION}}"
ZEPHYR_SDK_ARCHIVE_URL="${ZEPHYR_SDK_ARCHIVE_URL:-https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz}"

INSTALL_SYSTEM_DEPS=1
RUN_WEST_UPDATE=1
RUN_BUILD=1

APT_PACKAGES=(
	ca-certificates
	ccache
	cmake
	curl
	device-tree-compiler
	dfu-util
	file
	g++
	g++-multilib
	gcc
	gcc-multilib
	git
	gperf
	libmagic1
	libsdl2-dev
	make
	ninja-build
	python3
	python3-dev
	python3-tk
	xz-utils
)

usage() {
	cat <<EOF
Usage: $(basename "$0") [options]

Install the complete build environment and verify it by building both firmware
roles. Run this script as a normal user; it invokes sudo only for apt packages.

Options:
      --skip-system-deps  Do not run apt-get (no sudo required).
      --skip-update       Reuse the current west module checkouts.
      --skip-build        Install and validate the environment without building.
      --ncs-workspace DIR Override the NCS workspace directory.
      --sdk-dir DIR       Override the Zephyr SDK installation directory.
  -h, --help              Show this help.

Environment overrides:
  NCS_VERSION                 Default: ${NCS_VERSION}
  NCS_REPOSITORY              Default: ${NCS_REPOSITORY}
  NCS_WORKSPACE               Default: ${NCS_WORKSPACE}
  ZEPHYR_VENV                 Default: <NCS_WORKSPACE>/.venv
  PYTHON_VERSION              Default: ${PYTHON_VERSION}
  WEST_VERSION                Default: ${WEST_VERSION}
  ZEPHYR_SDK_VERSION          Default: ${ZEPHYR_SDK_VERSION}
  ZEPHYR_SDK_INSTALL_DIR      Default: ${ZEPHYR_SDK_INSTALL_DIR}
  ZEPHYR_SDK_ARCHIVE_URL      Override the Zephyr SDK download URL.
EOF
}

log() {
	printf '\n==> %s\n' "$*"
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--skip-system-deps)
			INSTALL_SYSTEM_DEPS=0
			shift
			;;
		--skip-update)
			RUN_WEST_UPDATE=0
			shift
			;;
		--skip-build)
			RUN_BUILD=0
			shift
			;;
		--ncs-workspace)
			[[ $# -ge 2 ]] || die "--ncs-workspace requires a directory"
			NCS_WORKSPACE="$2"
			ZEPHYR_VENV="${NCS_WORKSPACE}/.venv"
			shift 2
			;;
		--sdk-dir)
			[[ $# -ge 2 ]] || die "--sdk-dir requires a directory"
			ZEPHYR_SDK_INSTALL_DIR="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1"
			;;
	esac
done

[[ "$(uname -s)" == "Linux" ]] || die "only Linux hosts are currently supported"
[[ "$(uname -m)" == "x86_64" ]] || die "the default Zephyr SDK URL requires an x86_64 host"

if [[ ${EUID} -eq 0 && -n "${SUDO_USER:-}" ]]; then
	die "run this script as a normal user, not with sudo"
fi

if [[ ${INSTALL_SYSTEM_DEPS} -eq 1 ]]; then
	log "Installing system packages"
	if [[ ${EUID} -eq 0 ]]; then
		APT_PREFIX=()
	else
		command -v sudo >/dev/null 2>&1 || die "sudo is required; use --skip-system-deps after installing packages manually"
		APT_PREFIX=(sudo)
	fi
	"${APT_PREFIX[@]}" apt-get update
	"${APT_PREFIX[@]}" apt-get install -y --no-install-recommends "${APT_PACKAGES[@]}"
else
	log "Skipping system package installation"
fi

log "Installing uv and Python ${PYTHON_VERSION}"
UV_BIN="$(command -v uv || true)"
if [[ -z "${UV_BIN}" ]]; then
	mkdir -p "${HOME}/.local/bin"
	UV_INSTALLER="$(mktemp)"
	trap 'rm -f "${UV_INSTALLER:-}"' EXIT
	curl --fail --location --retry 3 --silent --show-error \
		https://astral.sh/uv/install.sh -o "${UV_INSTALLER}"
	UV_INSTALL_DIR="${HOME}/.local/bin" sh "${UV_INSTALLER}"
	UV_BIN="${HOME}/.local/bin/uv"
fi
[[ -x "${UV_BIN}" ]] || die "uv was not installed successfully"
"${UV_BIN}" python install "${PYTHON_VERSION}"

mkdir -p "${NCS_WORKSPACE}"
if [[ ! -x "${ZEPHYR_VENV}/bin/python" ]]; then
	log "Creating Python virtual environment at ${ZEPHYR_VENV}"
	"${UV_BIN}" venv --python "${PYTHON_VERSION}" --seed "${ZEPHYR_VENV}"
else
	VENV_PYTHON_VERSION="$("${ZEPHYR_VENV}/bin/python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
	[[ "${VENV_PYTHON_VERSION}" == "${PYTHON_VERSION}" ]] || \
		die "${ZEPHYR_VENV} uses Python ${VENV_PYTHON_VERSION}; expected ${PYTHON_VERSION}"
fi

log "Installing west ${WEST_VERSION}"
"${UV_BIN}" pip install --python "${ZEPHYR_VENV}/bin/python" "west==${WEST_VERSION}"
WEST="${ZEPHYR_VENV}/bin/west"
[[ -x "${WEST}" ]] || die "west was not installed into ${ZEPHYR_VENV}"

west_in_workspace() {
	(
		cd "${NCS_WORKSPACE}"
		"${WEST}" "$@"
	)
}

if [[ ! -d "${NCS_WORKSPACE}/.west" ]]; then
	log "Initializing nRF Connect SDK ${NCS_VERSION}"
	"${WEST}" init -m "${NCS_REPOSITORY}" --mr "${NCS_VERSION}" "${NCS_WORKSPACE}"
else
	[[ -d "${NCS_WORKSPACE}/nrf/.git" ]] || \
		die "${NCS_WORKSPACE} contains .west but no nrf Git repository"
	MANIFEST_PATH="$(west_in_workspace config manifest.path)"
	[[ "${MANIFEST_PATH}" == "nrf" ]] || \
		die "${NCS_WORKSPACE} uses manifest path '${MANIFEST_PATH}', expected 'nrf'"

	if ! git -C "${NCS_WORKSPACE}/nrf" rev-parse --verify --quiet "refs/tags/${NCS_VERSION}" >/dev/null; then
		log "Fetching NCS tag ${NCS_VERSION}"
		git -C "${NCS_WORKSPACE}/nrf" fetch origin "refs/tags/${NCS_VERSION}:refs/tags/${NCS_VERSION}"
	fi

	CURRENT_NCS_COMMIT="$(git -C "${NCS_WORKSPACE}/nrf" rev-parse HEAD)"
	EXPECTED_NCS_COMMIT="$(git -C "${NCS_WORKSPACE}/nrf" rev-list -n 1 "${NCS_VERSION}")"
	if [[ "${CURRENT_NCS_COMMIT}" != "${EXPECTED_NCS_COMMIT}" ]]; then
		[[ -z "$(git -C "${NCS_WORKSPACE}/nrf" status --porcelain)" ]] || \
			die "${NCS_WORKSPACE}/nrf has local changes; refusing to switch to ${NCS_VERSION}"
		log "Switching NCS manifest repository to ${NCS_VERSION}"
		git -C "${NCS_WORKSPACE}/nrf" checkout --detach "${NCS_VERSION}"
	fi
fi

if [[ ${RUN_WEST_UPDATE} -eq 1 ]]; then
	log "Downloading NCS modules (this can be resumed by rerunning the script)"
	GIT_TERMINAL_PROMPT=0 west_in_workspace update
else
	log "Skipping west update"
fi

[[ -f "${NCS_WORKSPACE}/nrfxlib/mpsl/include/mpsl_timeslot.h" ]] || \
	die "MPSL is missing; rerun without --skip-update"

log "Installing NCS Python packages"
west_in_workspace packages pip --install
west_in_workspace zephyr-export

SDK_COMPILER="${ZEPHYR_SDK_INSTALL_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"
if [[ ! -x "${SDK_COMPILER}" ]]; then
	if [[ ! -d "${ZEPHYR_SDK_INSTALL_DIR}" ]]; then
		log "Downloading Zephyr SDK ${ZEPHYR_SDK_VERSION}"
		SDK_CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/pps-env"
		SDK_ARCHIVE="${SDK_CACHE_DIR}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz"
		mkdir -p "${SDK_CACHE_DIR}"
		if [[ ! -s "${SDK_ARCHIVE}" ]]; then
			curl --fail --location --retry 3 --show-error \
				"${ZEPHYR_SDK_ARCHIVE_URL}" -o "${SDK_ARCHIVE}"
		fi

		SDK_TMP_DIR="$(mktemp -d)"
		trap 'rm -rf "${SDK_TMP_DIR:-}"; rm -f "${UV_INSTALLER:-}"' EXIT
		tar -xf "${SDK_ARCHIVE}" -C "${SDK_TMP_DIR}"
		EXTRACTED_SDK_DIR="${SDK_TMP_DIR}/zephyr-sdk-${ZEPHYR_SDK_VERSION}"
		[[ -x "${EXTRACTED_SDK_DIR}/setup.sh" ]] || \
			die "unexpected Zephyr SDK archive layout"
		mkdir -p "$(dirname "${ZEPHYR_SDK_INSTALL_DIR}")"
		mv "${EXTRACTED_SDK_DIR}" "${ZEPHYR_SDK_INSTALL_DIR}"
	fi

	[[ -x "${ZEPHYR_SDK_INSTALL_DIR}/setup.sh" ]] || \
		die "incomplete Zephyr SDK directory: ${ZEPHYR_SDK_INSTALL_DIR}"
	log "Installing ARM toolchain and Zephyr host tools"
	(
		cd "${ZEPHYR_SDK_INSTALL_DIR}"
		./setup.sh -t arm-zephyr-eabi -h -c
	)
fi
[[ -x "${SDK_COMPILER}" ]] || die "ARM Zephyr SDK toolchain is missing"

export NCS_WORKSPACE
export ZEPHYR_WORKSPACE="${NCS_WORKSPACE}"
export ZEPHYR_BASE="${NCS_WORKSPACE}/zephyr"
export ZEPHYR_VENV
export ZEPHYR_SDK_INSTALL_DIR

log "Environment summary"
printf 'NCS:        %s (%s)\n' "${NCS_WORKSPACE}" "$(git -C "${NCS_WORKSPACE}/nrf" describe --tags --exact-match HEAD)"
printf 'Python:     %s\n' "$("${ZEPHYR_VENV}/bin/python" --version 2>&1)"
printf 'west:       %s\n' "$("${WEST}" --version)"
printf 'Zephyr SDK: %s\n' "$("${SDK_COMPILER}" --version | head -n 1)"

if [[ ${RUN_BUILD} -eq 1 ]]; then
	log "Building master firmware"
	"${APP_DIR}/build.sh" master
	log "Building slave firmware"
	"${APP_DIR}/build.sh" slave
	printf '\nFirmware artifacts:\n'
	printf '  %s\n' "${APP_DIR}/build/master/zephyr/zephyr.uf2"
	printf '  %s\n' "${APP_DIR}/build/slave/zephyr/zephyr.uf2"
else
	log "Skipping firmware build"
fi

log "Environment setup completed successfully"
