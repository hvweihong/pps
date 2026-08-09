#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/ble_gatt_diagnostics_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

rg -q "gatt_tx_found" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_tx_ccc_found" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_subscribe_attempts" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_subscribe_failures" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_notify_subscribed" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_rx_found" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_write_attempts" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_write_failures" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_write_successes" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_write_completions" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_write_retries" "$repo_root/src/ble_time_sync_client.c"
rg -q "gatt_last_write_error" "$repo_root/src/ble_time_sync_client.c"
rg -q "bt_gatt_write_without_response_cb" "$repo_root/src/ble_time_sync_client.c"
rg -Fq "K_WORK_DELAYABLE_DEFINE(gatt_write_work" "$repo_root/src/ble_time_sync_client.c"
rg -q "bt_uuid_time_sync_rx" "$repo_root/src/ble_time_sync_uuids.c"
rg -q "bt_uuid_time_sync_tx" "$repo_root/src/ble_time_sync_uuids.c"
rg -q "src/ble_time_sync_uuids.c" "$repo_root/CMakeLists.txt"
rg -q "&bt_uuid_time_sync_rx.uuid" "$repo_root/src/ble_time_sync_client.c"
rg -q "&bt_uuid_time_sync_tx.uuid" "$repo_root/src/ble_time_sync_client.c"
rg -q "ble_time_sync_client_peer_count" "$repo_root/src/ble_time_sync_client.c"
rg -q "ble_time_sync_client_notify_subscribed" "$repo_root/src/ble_time_sync_client.c"
rg -q "\"ble_gatt:" "$repo_root/src/status.c"
rg -q "tx=%u ccc=%u sub=%u" "$repo_root/src/status.c"
rg -q "notify_sub=%u rx=%u write=%u" "$repo_root/src/status.c"

python3 - "$repo_root/src/ble_time_sync_client.c" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.find("static void connected_cb(")
if start < 0:
	raise SystemExit("missing connected_cb")
scan_after_connected = source.find("LOG_INF(\"BLE central connected\")", start)
if scan_after_connected < 0:
	raise SystemExit("missing central connected log")
next_func = source.find("static void disconnected_cb(", scan_after_connected)
if next_func < 0:
	raise SystemExit("missing disconnected_cb")
body_after_connected = source[scan_after_connected:next_func]
if "scan_start();" in body_after_connected:
	raise SystemExit("central must not restart scanning after a peer connects")
PY

python3 - "$repo_root/src/status.c" <<'PY'
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
strings = "".join(re.findall(r'"([^"]*)"', source))
for required in (
	"write_ok=%u write_sent=%u write_fail=%u",
	"write_retry=%u write_inflight=%u write_step=%u write_last_err=%d",
):
	if required not in strings:
		raise SystemExit(f"missing status field sequence: {required}")
PY
