#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
radio_c = (root / "src/radio_sync.c").read_text()
radio_h = (root / "src/radio_sync.h").read_text()
main_c = (root / "src/main.c").read_text()
status_c = (root / "src/status.c").read_text()
kconfig = (root / "Kconfig").read_text()

required_stats = [
    "tx_api_busy_errors",
    "tx_mpsl_request_errors",
    "tx_mpsl_blocked",
    "tx_mpsl_cancelled",
    "tx_prepare_late",
]
for field in required_stats:
    if field not in radio_h:
        raise SystemExit(f"radio stats must expose {field}")
    if f"radio->{field}" not in status_c:
        raise SystemExit(f"status output must include {field}")

for symbol in [
    "TIME_SYNC_MASTER_TX_TIMESLOT_LENGTH_US",
    "TIME_SYNC_MASTER_TX_START_OFFSET_US",
]:
    if f"config {symbol}" not in kconfig:
        raise SystemExit(f"missing Kconfig symbol {symbol}")

if "int radio_sync_start_master(" not in radio_h:
    raise SystemExit("master must start RADIO TX through a periodic radio_sync API")

if "radio_sync_start_master(" not in main_c:
    raise SystemExit("master loop must start periodic RADIO TX once")

if "radio_sync_send_beacon_at(" in main_c:
    raise SystemExit("master loop must not synchronously request a timeslot per beacon")

if "MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST" not in radio_c:
    raise SystemExit("master periodic TX must chain the next NORMAL timeslot from callback")

if "TIMESLOT_MASTER_TX_ACTIVE" not in radio_c:
    raise SystemExit("master TX needs a persistent active mode distinct from one-shot TX")

if not re.search(
    r"timeslot_request_normal\.params\.normal\.distance_us\s*=\s*"
    r"master_tx_cfg\.sync_interval_us",
    radio_c,
):
    raise SystemExit("master NORMAL request distance must be the configured sync interval")

if not re.search(
    r"timeslot_request_normal\.params\.normal\.length_us\s*=\s*master_tx_timeslot_length_us\(",
    radio_c,
):
    raise SystemExit("master TX NORMAL request must use the short master TX timeslot length")

if "complete_tx_from_callback(-EBUSY)" in radio_c:
    raise SystemExit("BLOCKED/CANCELLED must not be collapsed into generic -EBUSY")
PY
