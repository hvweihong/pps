#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

radio = (Path.cwd() / "src" / "radio_sync.c").read_text()

defines = dict(re.findall(
    r"#define\s+(TIMESLOT_\w+_CHANNEL)\s+(NRF_TIMER_CC_CHANNEL\d+)", radio
))

required = {
    "TIMESLOT_END_TIMER_CHANNEL",
    "TIMESLOT_TX_TIMEOUT_CHANNEL",
    "TIMESLOT_CAPTURE_CHANNEL",
    "TIMESLOT_TXEN_TIMER_CHANNEL",
    "TIMESLOT_RXEN_TIMER_CHANNEL",
}
missing = required - set(defines)
if missing:
    raise SystemExit(f"missing timer channel define(s): {sorted(missing)}")

for name, channel in defines.items():
    number = int(channel.removeprefix("NRF_TIMER_CC_CHANNEL"))
    if number > 3:
        raise SystemExit(
            f"{name} uses {channel}, but nRF52840 MPSL TIMER0 has only CC0..CC3"
        )

if defines["TIMESLOT_TXEN_TIMER_CHANNEL"] == defines["TIMESLOT_TX_TIMEOUT_CHANNEL"]:
    raise SystemExit("TXEN compare must not be overwritten by TX timeout compare")

if defines["TIMESLOT_RXEN_TIMER_CHANNEL"] != defines["TIMESLOT_TX_TIMEOUT_CHANNEL"]:
    raise SystemExit("RXEN should reuse the timeout channel only in RX mode")
PY
