#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
kconfig = (root / "Kconfig").read_text()
main = (root / "src/main.c").read_text()
readme = (root / "README.md").read_text()
radio = (root / "src/radio_sync.c").read_text()

def default_for(symbol: str) -> int:
    match = re.search(
        rf"config {symbol}\n(?:\t.*\n)*?\tdefault ([0-9]+)",
        kconfig,
    )
    if not match:
        raise SystemExit(f"missing default for {symbol}")
    return int(match.group(1))

def role_default_for(symbol: str, role: str) -> int:
    match = re.search(
        rf"config {symbol}\n(?:\t.*\n)*?\tdefault ([0-9]+) if {role}",
        kconfig,
    )
    if not match:
        raise SystemExit(f"missing default for {symbol} if {role}")
    return int(match.group(1))

sync_interval = default_for("TIME_SYNC_INTERVAL_US")
master_timeslot = role_default_for("TIME_SYNC_TIMESLOT_LENGTH_US", "TIME_SYNC_ROLE_MASTER")
slave_timeslot = role_default_for("TIME_SYNC_TIMESLOT_LENGTH_US", "TIME_SYNC_ROLE_SLAVE")
acquire_window = default_for("TIME_SYNC_RX_ACQUIRE_WINDOW_US")

if master_timeslot >= sync_interval // 2:
	raise SystemExit(
		"master TX timeslot must stay well below the sync interval for BLE"
	)

if slave_timeslot < sync_interval:
	raise SystemExit(
		"slave RADIO-first acquisition needs a full-interval timeslot before "
		"BLE is started"
	)

if acquire_window < sync_interval:
	raise SystemExit(
		"slave acquisition must keep a full sync-period RX window until lock"
	)

if "next_status = now + TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000ULL;" not in main:
	raise SystemExit("master status scheduling must skip stale intervals instead of catch-up logging")

if "MASTER_BLE_START_SUCCESS_BEACONS" not in main:
	raise SystemExit("master must delay BLE until RADIO TX is stable")

if "config TIME_SYNC_MASTER_BLE_AUTO_START" not in kconfig:
	raise SystemExit("master BLE auto-start must be configurable")

if re.search(
	r"config TIME_SYNC_MASTER_BLE_AUTO_START\n(?:\t.*\n)*?\tdefault y",
	kconfig,
):
	raise SystemExit("master BLE auto-start must be disabled by default for RADIO-only validation")

if "CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START" not in main:
	raise SystemExit("master BLE start must be behind CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START")

if "select_tx_timeslot_request" in radio:
	raise SystemExit("master TX must not keep a long pending NORMAL timeslot request")

expected = (
	f"| `CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US` | MPSL timeslot 长度 | "
	f"master `{master_timeslot}`，slave `{slave_timeslot}` |"
)
if expected not in readme:
	raise SystemExit("README timeslot length default is not in sync with Kconfig")
PY
