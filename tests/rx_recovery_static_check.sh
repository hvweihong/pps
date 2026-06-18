#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
main = (root / "src/main.c").read_text()
radio = (root / "src/radio_sync.c").read_text()
kconfig = (root / "Kconfig").read_text()

if "ret != 0 && ret != -NRF_EAGAIN" in radio:
    raise SystemExit("MPSL -NRF_EAGAIN must not be treated as a successful RX request")

if "mode == RADIO_SYNC_RX_ACQUIRE" not in radio or "TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE" not in radio:
    raise SystemExit("acquire RX must use a grantable earliest timeslot with the full request timeout")

for token in [
    "timeslot_anchor_valid",
    "select_rx_timeslot_request",
    "requested_rx_window_length_us",
    "requested_rx_timeslot_length_us",
    "rx_request_active",
]:
    if token not in radio:
        raise SystemExit(f"radio_sync.c missing scheduled RX support token: {token}")

if "rx_request_active" not in (root / "src/radio_sync.h").read_text():
    raise SystemExit("radio stats must expose whether an RX request is still active")

if "slave_predict_master_to_local" not in main:
    raise SystemExit("slave must be able to predict master TX in acquiring/holdover states")

if "next_expected_master_tx" not in main:
    raise SystemExit("slave must track the next expected master TX tick across missed windows")

if "!radio_stats->rx_request_active" not in main:
    raise SystemExit("slave must recover immediately when an RX timeslot is blocked/cancelled")

if "slave_note_missed_rx_window" not in main:
    raise SystemExit("slave missed-window recovery must be shared for timeout and MPSL block")

missed_branch = re.search(
    r"consecutive_missed_windows\s*>=\s*TIME_SYNC_RX_MISSED_TO_ACQUIRE_VALUE(?P<body>.*?)else",
    main,
    re.S,
)
if not missed_branch:
    raise SystemExit("missing missed-window fallback branch")

body = missed_branch.group("body")
if "sync_filter_init" not in body or "RADIO_SYNC_RX_ACQUIRE" not in body:
    raise SystemExit("missed-window fallback must clear old filter state and return to acquire")

lock_beacons = re.search(r"\.lock_beacons\s*=\s*([0-9]+)", (root / "src/sync_filter.c").read_text())
missed_to_acquire = re.search(
    r"config TIME_SYNC_RX_MISSED_TO_ACQUIRE\n(?:\t.*\n)*?\tdefault ([0-9]+)",
    kconfig,
)
if not lock_beacons or not missed_to_acquire:
    raise SystemExit("missing lock/missed defaults")

if int(missed_to_acquire.group(1)) >= int(lock_beacons.group(1)):
    raise SystemExit("recovery must return to acquire before a stale filter can look locked")
PY
