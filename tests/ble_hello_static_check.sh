#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/ble_hello_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "HELLO_FROM_MASTER hello world" "$repo_root/src/ble_time_sync_client.c"
rg -q "HELLO_FROM_SLAVE hello world" "$repo_root/src/main.c"
rg -q "BLE hello from master" "$repo_root/src/main.c"
rg -q "CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START=y" "$repo_root/master.conf"
rg -q "notify_subscribed" "$repo_root/src/ble_time_sync_client.c"
rg -q "subscribe_params.subscribe" "$repo_root/src/ble_time_sync_client.c"
rg -q "BLE notify subscribe complete" "$repo_root/src/ble_time_sync_client.c"
rg -q "BLE RX command:" "$repo_root/src/ble_time_sync_service.c"
rg -q "BLE TX notify enabled" "$repo_root/src/ble_time_sync_service.c"
