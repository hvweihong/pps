#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

root = Path.cwd()
overlay = (root / "boards/xiao_ble_nrf52840.overlay").read_text()
timebase_h = (root / "src/timebase.h").read_text()
timebase_c = (root / "src/timebase.c").read_text()
source = (root / "src/pps_input.c").read_text()
header = (root / "src/pps_input.h").read_text()
cmake = (root / "CMakeLists.txt").read_text()

def require(pattern, text, message):
    if not re.search(pattern, text, re.S):
        raise SystemExit(message)

require(r"pps-in\s*=\s*&pps_in\s*;", overlay,
        "pps-in must alias pps_in exactly")
require(r"pps_in\s*:\s*pps_in\s*\{[^}]*gpios\s*=\s*<&gpio0\s+2\s+GPIO_ACTIVE_HIGH>",
        overlay, "pps_in must be active-high GPIO P0.02")
require(r"#define\s+TIMEBASE_CHANNEL_COUNT\s+6u", timebase_c,
        "TIMER3 must expose six channels")
require(r"#define\s+TIMEBASE_PPS_INPUT_CAPTURE_CHANNEL\s+4u", timebase_h,
        "PPS input must own TIMER3 CC4")
require(r"DT_ALIAS\(pps_in\)", source, "pps input must use DT_ALIAS(pps_in)")
require(r"timebase_timer_capture_task_address\(\s*TIMEBASE_PPS_INPUT_CAPTURE_CHANNEL\s*\)",
        source, "PPS input must route GPPI to TIMER3 CC4 capture")
require(r"nrfx_gpiote_channel_alloc", source,
        "PPS input must allocate its GPIOTE channel through Zephyr's nrfx instance")
require(r"nrfx_gpiote_input_configure", source,
        "PPS input must register an nrfx GPIOTE input handler")
require(r"NRFX_GPIOTE_TRIGGER_LOTOHI", source,
        "PPS input must capture low-to-high edges")
require(r"nrfx_gppi_conn_alloc", source,
        "PPS input must allocate a GPPI connection")
require(r"K_MSGQ_DEFINE\([^\n]+", source,
        "PPS input requires a bounded message queue")
require(r"k_msgq_put\([^\n]+K_NO_WAIT", source,
        "PPS ISR must enqueue without blocking")
require(r"dropped_count", source, "PPS queue overflow must be counted")
require(r"int\s+pps_input_init\s*\(void\)", header,
        "missing pps_input_init API")
require(r"int\s+pps_input_poll\s*\(uint64_t\s*\*[^,]+,\s*k_timeout_t", header,
        "missing pps_input_poll API")
require(r"uint64_t\s+pps_input_count\s*\(void\)", header,
        "missing pps_input_count API")
require(r"src/pps_input\.c", cmake, "pps input source is not built")

isr = re.search(r"static void pps_input_isr\([^)]*\)[^{]*\{(.*?)^\}",
                source, re.S | re.M)
if isr is None:
    raise SystemExit("missing PPS input ISR")
if re.search(r"nmea|settings|pps_output|pps_input_poll|LOG_|k_msgq_get|k_sleep",
             isr.group(1), re.I):
    raise SystemExit("PPS ISR may only capture, enqueue, and count")
if re.search(r"IRQ_CONNECT|NRF_GPIOTE0_IRQn", source):
    raise SystemExit("PPS input must use Zephyr's shared nrfx GPIOTE IRQ")
PY

echo "PPS input static check: PASS"
