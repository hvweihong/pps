#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
param="${root}/src/param_config.c"
header="${root}/src/param_config.h"
shell="${root}/src/param_shell.c"
main="${root}/src/main.c"
runtime="${root}/src/bridge_runtime.c"
runtime_header="${root}/src/bridge_runtime.h"
uart="${root}/src/uart_bridge.c"
uart_header="${root}/src/uart_bridge.h"
heartbeat="${root}/src/heartbeat_led.c"

rg -q 'rb_param_persistence_available' "$param" "$header" "$main"
rg -q 'K_MUTEX_DEFINE\(param_mutex\)' "$param"
if rg -q 'k_spin_(lock|unlock)|k_spinlock' "$param"; then
	echo "settings persistence must not run under a spinlock" >&2
	exit 1
fi
rg -q 'RB_PARAM_BYTES' "$shell"
rg -q 'rb_group_key_parse' "$shell"
rg -q 'rb_param_set_bytes' "$shell"
rg -q 'int[[:space:]]+uart_bridge_init\(uint32_t[[:space:]]+baudrate\)' "$uart_header"
rg -q 'uart_bridge_init\(uart_baud' "$runtime"
rg -Uq 'wireless_time_sync_init\([^;]*radio_delay_us' "$runtime"
rg -Uq 'pps_output_start_periodic\([^;]*1000000u\)' "$main"
if rg -q 'rb_param_get_uint32\(RB_PARAM_PPS_PERIOD_US' "$main" "$runtime"; then
	echo "PPS period must be fixed at 1 Hz" >&2
	exit 1
fi
rg -q 'heartbeat_led_update' "$main" "$heartbeat"
rg -q 'RB_PARAM_STATUS_INTERVAL_MS' "$main"
rg -q 'rb_bridge_runtime_accept_sync_group' "$runtime" "$runtime_header"
rg -q 'invalid_group_packets\+\+' "$runtime_header"

echo "parameter fallback and consumer wiring static check: PASS"
