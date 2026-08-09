#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/ble_status_diagnostics_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "\"ble_link:" "$repo_root/src/status.c"
rg -q "\"ble_scan:" "$repo_root/src/status.c"
rg -q "\"ble_gatt:" "$repo_root/src/status.c"
rg -q "started=%u" "$repo_root/src/status.c"
rg -q "ready=%u" "$repo_root/src/status.c"
rg -q "scan=%u" "$repo_root/src/status.c"
rg -q "adv=%u" "$repo_root/src/status.c"
rg -q "peer=%u" "$repo_root/src/status.c"
rg -q "gate=%u" "$repo_root/src/status.c"
rg -q "last_err=%d" "$repo_root/src/status.c"
rg -q "start_attempts=%u" "$repo_root/src/status.c"
rg -q "ble_time_sync_snapshot" "$repo_root/src/ble_time_sync.h"
rg -q "ble_time_sync_get_snapshot" "$repo_root/src/ble_time_sync.c"
rg -q "ble_time_sync_client_scanning" "$repo_root/src/ble_time_sync_client.c"
rg -q "ble_advertising" "$repo_root/src/ble_time_sync.c"
rg -q "ble_time_sync_service_notify_enabled" "$repo_root/src/ble_time_sync_service.c"
rg -q "stable_tx_count" "$repo_root/src/main.c"
rg -q "locked_beacon_count" "$repo_root/src/main.c"
