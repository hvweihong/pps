#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
ble = (root / "src/ble_time_sync.c").read_text()
header = (root / "src/ble_time_sync.h").read_text()
main = (root / "src/main.c").read_text()

def c_function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"missing {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise SystemExit(f"missing function body for {signature}")
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:pos]
    raise SystemExit(f"unterminated function body for {signature}")

init_body = c_function_body(ble, "int ble_time_sync_init(void)")
if "slave_advertising_start()" in init_body:
    raise SystemExit("ble_time_sync_init() must not start slave advertising")

if "ble_time_sync_client_start()" in init_body:
    raise SystemExit("ble_time_sync_init() must not start master scanning")

if "bt_enable(" in init_body:
    raise SystemExit(
        "ble_time_sync_init() must not call bt_enable(); the BT controller "
        "must stay off until RADIO sync is stable"
    )

start_body = c_function_body(ble, "int ble_time_sync_start(void)")
enable_body = c_function_body(ble, "static int ble_time_sync_enable_stack(void)")
if "ble_time_sync_enable_stack()" not in start_body or "bt_enable(" not in enable_body:
    raise SystemExit("ble_time_sync_start() must enable the BT stack lazily")

if "int ble_time_sync_start(void);" not in header:
    raise SystemExit("ble_time_sync_start() must be exposed")

if "bool ble_time_sync_started(void);" not in header:
    raise SystemExit("ble_time_sync_started() must be exposed")

if main.count("ble_time_sync_start()") < 2:
    raise SystemExit("master and slave loops must have explicit BLE start gates")

if "MASTER_BLE_START_SUCCESS_BEACONS" not in main:
    raise SystemExit("master must gate BLE start on consecutive successful RADIO TX")

if "CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START" not in main:
    raise SystemExit("master BLE scan must be disabled by default during RADIO-only validation")

if "SLAVE_BLE_START_LOCKED_BEACONS" not in main:
    raise SystemExit("slave must gate BLE start on locked RADIO observations")

if "BLE controller 还未 `bt_enable()`" not in (root / "README.md").read_text():
    raise SystemExit("README must document that BT enable is delayed until RADIO is stable")
PY
