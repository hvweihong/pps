#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

radio = (Path.cwd() / "src" / "radio_sync.c").read_text()

if "static int arm_rx_window_end_timer(uint64_t rxen_slot_tick)" not in radio:
    raise SystemExit("missing RX window end timer helper")

prepare_start = radio.index("static int prepare_rx_in_timeslot(void)")
prepare_end = radio.index("#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)", prepare_start)
prepare_body = radio[prepare_start:prepare_end]

if "arm_rx_window_end_timer(slot_rxen)" in prepare_body:
    raise SystemExit(
        "prepare_rx_in_timeslot must not arm window end before RXEN when CC1 is reused"
    )

trigger_start = radio.index("static void trigger_rxen_now(void)")
trigger_end = radio.index("static int arm_rx_window_end_timer", trigger_start)
trigger_body = radio[trigger_start:trigger_end]

if "arm_rx_window_end_timer(" not in trigger_body:
    raise SystemExit("RXEN callback must reprogram CC1 for RX window end")

if "stats.rx_window_late++" not in trigger_body:
    raise SystemExit("RXEN callback must account for failed RX window end arming")
PY
