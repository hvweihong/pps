#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "\"sync_status:" "$repo_root/src/status.c"
rg -q "\"pps_status:" "$repo_root/src/status.c"
rg -q "\"radio_tx_status:" "$repo_root/src/status.c"
rg -q "\"radio_rx_status:" "$repo_root/src/status.c"
rg -q "\"timeslot_status:" "$repo_root/src/status.c"
rg -q "\"ble_link:" "$repo_root/src/status.c"
rg -q "\"ble_scan:" "$repo_root/src/status.c"
rg -q "\"ble_gatt:" "$repo_root/src/status.c"

if rg -q "\"ble_status:" "$repo_root/src/status.c"; then
	echo "BLE status must be split into ble_link/ble_scan/ble_gatt"
	exit 1
fi
