#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
kconfig = (root / "Kconfig").read_text()
app_config = (root / "src/app_config.h").read_text()
radio = (root / "src/radio_sync.c").read_text()
main = (root / "src/main.c").read_text()

if "config TIME_SYNC_RX_START_OFFSET_US" not in kconfig:
    raise SystemExit("RX needs a configurable in-timeslot RXEN start offset")

if "TIME_SYNC_RX_START_OFFSET_US_VALUE" not in app_config:
    raise SystemExit("app_config must expose TIME_SYNC_RX_START_OFFSET_US")

if "rx_start_offset_us()" not in radio:
    raise SystemExit("radio_sync must reserve RXEN prep time inside every RX timeslot")

if not re.search(
    r"length\s*=\s*rx_start_offset_us\(\)\s*\+\s*window_len\s*\+",
    radio,
):
    raise SystemExit("RX timeslot length must include RX start offset before the window")

if "TIME_SYNC_RX_WINDOW_GUARD_US_VALUE" not in radio:
    raise SystemExit("RX timeslot length must include configured window guard slack")

if "requested_rx_start_immediate =" in radio:
    raise SystemExit("acquire RX must not open immediately at timeslot start")

if re.search(r"rx_window_missed\s*=.*\|\|", main, re.S):
    raise SystemExit("slave must not mark an RX window missed while MPSL request is still active")

if "finish_rx_window_from_callback();" not in radio:
    raise SystemExit("radio callback must be able to finish RX windows")

rx_radio_branch = re.search(
    r"timeslot_radio_action\(void\)(?P<body>.*?)return none_from_callback",
    radio,
    re.S,
)
if not rx_radio_branch or "rx_packet_received" not in rx_radio_branch.group("body"):
    raise SystemExit("valid RX packets must end the current RX window immediately")
PY
