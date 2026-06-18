#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "BT_GAP_SCAN_FAST_INTERVAL" "$repo_root/src/ble_time_sync_client.c"
rg -q "BT_GAP_SCAN_FAST_WINDOW" "$repo_root/src/ble_time_sync_client.c"
rg -q "scan_seen" "$repo_root/src/ble_time_sync_client.c"
rg -q "scan_match" "$repo_root/src/ble_time_sync_client.c"
rg -q "scan_reject_type" "$repo_root/src/ble_time_sync_client.c"
rg -q "scan_reject_filter" "$repo_root/src/ble_time_sync_client.c"
rg -q "connect_attempts" "$repo_root/src/ble_time_sync_client.c"
rg -q "connect_failures" "$repo_root/src/ble_time_sync_client.c"
rg -q "ble_time_sync_client_get_scan_stats" "$repo_root/src/ble_time_sync_client.h"
rg -q "BT_GAP_ADV_FAST_INT_MIN_2" "$repo_root/src/ble_time_sync.c"
rg -q "BT_GAP_ADV_FAST_INT_MAX_2" "$repo_root/src/ble_time_sync.c"
rg -q "ble_scan_seen" "$repo_root/src/status.c"
rg -q "ble_scan_match" "$repo_root/src/status.c"
rg -q "ble_scan_type_drop" "$repo_root/src/status.c"
rg -q "ble_scan_filter_drop" "$repo_root/src/status.c"
rg -q "ble_conn_req" "$repo_root/src/status.c"
rg -q "ble_conn_fail" "$repo_root/src/status.c"
rg -q "ble_scan_seen" "$repo_root/README.md"
rg -q "ble_conn_fail" "$repo_root/README.md"
