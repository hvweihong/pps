#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "ble_started" "$repo_root/src/status.c"
rg -q "ble_ready" "$repo_root/src/status.c"
rg -q "ble_scan" "$repo_root/src/status.c"
rg -q "ble_adv" "$repo_root/src/status.c"
rg -q "ble_peer" "$repo_root/src/status.c"
rg -q "ble_gate" "$repo_root/src/status.c"
rg -q "ble_last_err" "$repo_root/src/status.c"
rg -q "ble_start_attempts" "$repo_root/src/status.c"
rg -q "ble_time_sync_snapshot" "$repo_root/src/ble_time_sync.h"
rg -q "ble_time_sync_get_snapshot" "$repo_root/src/ble_time_sync.c"
rg -q "ble_time_sync_client_scanning" "$repo_root/src/ble_time_sync_client.c"
rg -q "ble_advertising" "$repo_root/src/ble_time_sync.c"
rg -q "ble_time_sync_service_notify_enabled" "$repo_root/src/ble_time_sync_service.c"
rg -q "stable_tx_count" "$repo_root/src/main.c"
rg -q "locked_beacon_count" "$repo_root/src/main.c"
