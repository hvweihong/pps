#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re

root = Path.cwd()
overlay = (root / "boards/xiao_ble_nrf52840.overlay").read_text()
source = (root / "src/time_uart.c").read_text()
header = (root / "src/time_uart.h").read_text()
cmake = (root / "CMakeLists.txt").read_text()
kconfig = (root / "Kconfig").read_text()
prj = (root / "prj.conf").read_text()
params_h = (root / "src/param_defs.h").read_text()
params_c = (root / "src/param_config.c").read_text()
runtime = (root / "src/bridge_runtime.c").read_text()
main = (root / "src/main.c").read_text()

def require(pattern, text, message):
    if not re.search(pattern, text, re.S):
        raise SystemExit(message)

require(r"time-uart\s*=\s*&uart1\s*;", overlay,
        "time-uart must alias uart1 exactly")
require(r"&uart1\s*\{[\s\S]*?current-speed\s*=\s*<9600>\s*;", overlay,
        "UART1 devicetree default speed must be 9600")
require(r"uart1_default\s*:\s*uart1_default\s*\{[\s\S]*?NRF_PSEL\(UART_TX,\s*0,\s*4\)[\s\S]*?NRF_PSEL\(UART_RX,\s*0,\s*5\)",
        overlay, "UART1 default pinctrl must use TX P0.04 and RX P0.05")
require(r"bias-pull-up", overlay, "UART1 RX P0.05 requires pull-up")
require(r"uart1_sleep\s*:\s*uart1_sleep", overlay,
        "UART1 requires low-power sleep pinctrl")
require(r"low-power-enable", overlay,
        "UART1 sleep pinctrl must enable low power")
require(r"DT_ALIAS\(time_uart\)", source, "time UART must use DT_ALIAS(time_uart)")
require(r"\.baudrate\s*=\s*baudrate", source, "time UART must use requested baudrate")
for setting in ("UART_CFG_PARITY_NONE", "UART_CFG_STOP_BITS_1", "UART_CFG_DATA_BITS_8", "UART_CFG_FLOW_CTRL_NONE"):
    require(setting, source, f"time UART must configure 8N1 ({setting})")
require(r"rx_dma_buf\s*\[\s*2\s*\]", source,
        "time UART must own exactly two RX DMA buffers")
require(r"UART_RX_BUF_REQUEST", source, "time UART must replace RX buffers")
require(r"UART_RX_DISABLED", source, "time UART must account for/restart disabled RX")
require(r"line_overflow", source, "time UART must drop overlong lines whole")
require(r"[\\r\\n]", source, "time UART must delimit CR/LF lines")
require(r"int\s+time_uart_init\s*\(uint32_t\s+baudrate\)", header,
        "missing time_uart_init API")
require(r"size_t\s+time_uart_read_line\s*\(char\s*\*[^,]+,\s*size_t", header,
        "missing time_uart_read_line API")
require(r"time_uart_stats_get", header, "missing time UART statistics API")
require(r"K_MUTEX_DEFINE\(line_lock\)", source,
        "time UART line mutex must be statically initialized")
require(r"typedef\s+void\s+\(\*rb_time_uart_wake_fn\)\(void\)", header,
        "missing time UART wake callback type")
require(r"void\s+time_uart_set_wake_callback\s*\(rb_time_uart_wake_fn", header,
        "missing time UART wake callback API")
require(r"wake_callback\s*\(\s*\)", source,
        "time UART RX callback must wake its consumer after copying bytes")
require(r"src/time_uart\.c", cmake, "time UART source is not built")
require(r"config\s+TIME_UART_BAUDRATE[\s\S]*?default\s+9600[\s\S]*?range\s+1200\s+115200",
        kconfig, "time UART Kconfig baud range/default are wrong")
require(r"CONFIG_UART_1_ASYNC=y", prj, "UART1 async API must be enabled")
require(r"CONFIG_UART_1_INTERRUPT_DRIVEN=n", prj,
        "UART1 interrupt-driven mode must be off for async UARTE")
require(r"RB_PARAM_TIME_UART_BAUDRATE", params_h, "missing time UART parameter ID")
require(r"\.name\s*=\s*\"time_uart_baudrate\"[\s\S]*?RB_PARAM_FLAG_REBOOT_REQUIRED[\s\S]*?\.min\s*=\s*1200[\s\S]*?\.max\s*=\s*115200[\s\S]*?\.default_value\s*=\s*CONFIG_TIME_UART_BAUDRATE",
        params_c, "time UART parameter must be reboot-effective 1200..115200")
require(r"#include\s+\"time_uart\.h\"", runtime,
        "bridge runtime must own time UART initialization")
require(r"rb_param_get_uint32\(RB_PARAM_TIME_UART_BAUDRATE,\s*&time_uart_baud\)", runtime,
        "bridge runtime must read the reboot-effective time UART baudrate")
require(r"time_uart_set_wake_callback\(bridge_thread_wake\);[\s\S]*?time_uart_init\(time_uart_baud\)",
        runtime, "bridge runtime must register wake callback before enabling time UART RX")
if re.search(r"time_uart_(?:init|set_wake_callback)", main):
    raise SystemExit("main must not initialize time UART outside the bridge runtime owner")

callback = re.search(r"static void time_uart_callback\([^)]*\)[^{]*\{(.*?)^\}",
                     source, re.S | re.M)
if callback is None:
    raise SystemExit("missing time UART callback")
if re.search(r"nmea|utc_clock|settings|LOG_|k_mutex|k_sem_take|k_msgq_get|k_sleep",
             callback.group(1), re.I):
    raise SystemExit("time UART callback may only copy and wake/restart RX")
PY

echo "Time UART static check: PASS"
