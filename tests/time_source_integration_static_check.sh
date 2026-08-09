#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "${ROOT}" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
runtime = (root / "src/bridge_runtime.c").read_text()
main = (root / "src/main.c").read_text()
status = (root / "src/status.c").read_text()
shell = (root / "src/bridge_validation_shell.c").read_text()

apply_match = re.search(
    r"static void runtime_apply_sync\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    runtime,
    re.S,
)
if not apply_match:
    raise SystemExit("runtime_apply_sync body not found")
apply_body = apply_match.group("body")
group_guard = apply_body.find("frame.group_id != runtime_group_id")
if group_guard < 0:
    raise SystemExit("runtime_apply_sync must reject a mismatched group")
for mutation in (
    "wireless_sync.session",
    "sync_filter_reset",
    "rb_scheduler_set_discovery_slot",
    "bridge_stats.sync_rx_count",
    "wireless_time_sync_slave_receive",
):
    position = apply_body.find(mutation)
    if position < 0 or position < group_guard:
        raise SystemExit(f"group gate must precede {mutation}")
if "runtime_apply_wireless_utc" not in apply_body:
    raise SystemExit("accepted wireless frames must apply UTC publication")

required_runtime_tokens = (
    "rb_utc_clock_init",
    "pps_input_init",
    "pps_input_poll",
    "time_uart_read_line",
    "rb_nmea_parse_sentence",
    "rb_nmea_utc_to_unix",
    "rb_utc_clock_note_pair",
    "rb_scheduler_set_time_publication",
    "sync_filter_note_missed",
    "sync_filter_age",
    "target_tick < now_tick + 100u",
)
for token in required_runtime_tokens:
    if token not in runtime:
        raise SystemExit(f"bridge runtime missing {token}")
if runtime.find("rb_scheduler_set_time_publication") > runtime.find(
    "rb_scheduler_next_action"
):
    raise SystemExit("UTC publication must update before the next scheduler action")
if "pps_input_init" in main:
    raise SystemExit("PPS input must initialize after PPS output in bridge runtime")
external_start = runtime.find(
    "if (runtime_time_source_mode == 1u && config.master) {"
)
external_end = runtime.find("current_profile =", external_start)
if external_start < 0 or external_end < 0:
    raise SystemExit("external-master initialization block missing")
external_init_body = runtime[external_start:external_end]
for token in ("pps_input_init", "time_uart_init"):
    if token not in external_init_body or runtime.count(token) != 1:
        raise SystemExit(f"{token} must initialize only for the external master")

for token in (
    "time_source=",
    "utc_quality=",
    "utc_seconds=",
    "external_pps_count=",
    "nmea_valid_count=",
    "nmea_drop_count=",
    "holdover_count=",
    "sync_age_us=",
):
    if token not in status:
        raise SystemExit(f"status output missing {token}")
for token in ("time_test", "source_lost", "bridge_runtime_validation_time_pair"):
    if token not in shell:
        raise SystemExit(f"validation shell missing {token}")
PY

echo "time source integration static checks passed"
