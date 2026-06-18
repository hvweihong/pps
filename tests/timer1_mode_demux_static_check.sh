#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

radio = (Path.cwd() / "src" / "radio_sync.c").read_text()

timer_action = re.search(
    r"static mpsl_timeslot_signal_return_param_t \*timeslot_timer_action\(void\)\n\{(?P<body>.*?)\n\}",
    radio,
    re.S,
)
if not timer_action:
    raise SystemExit("missing timeslot_timer_action")

body = timer_action.group("body")
rxen_check = body.find("nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL)")
compare1_check = body.find("NRF_TIMER_EVENT_COMPARE1")

if rxen_check < 0 or compare1_check < 0:
    raise SystemExit("timer action must handle RXEN and compare1 timeout")

prefix = body[:rxen_check]
if "requested_mode == TIMESLOT_MODE_RX" not in prefix:
    raise SystemExit("RXEN compare1 event must only be consumed in RX mode")

if rxen_check > compare1_check:
    raise SystemExit("RXEN compare must be handled before RX window end in RX mode")
PY
