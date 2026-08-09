#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/firmware_identity_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "APP_LOG_FORMAT_VERSION" "$repo_root/src/main.c"
rg -q "\"firmware:" "$repo_root/src/main.c"
rg -q "gatt_diag=1" "$repo_root/src/main.c"
