#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FILE="${ROOT}/src/bridge_runtime.c"
[[ -f "${FILE}" ]]
grep -q 'K_THREAD_STACK_DEFINE' "${FILE}"
grep -q 'K_MSGQ_DEFINE' "${FILE}"
grep -q 'radio_transport_get_event' "${FILE}"
grep -q 'uart_bridge_read' "${FILE}"
grep -q 'uart_bridge_write' "${FILE}"
grep -q 'uart_bridge_write_record' "${FILE}"
grep -q 'uart_bridge_record_available' "${FILE}"
grep -q 'rb_scheduler_master_peek_record' "${FILE}"
grep -q 'rb_scheduler_master_pop_record' "${FILE}"
if grep -q 'rb_scheduler_master_read_uart' "${FILE}"; then
	echo "master runtime must not destructively read merged UART bytes" >&2
	exit 1
fi
if ! grep -Pzq 'rb_scheduler_master_peek_record[\s\S]*uart_bridge_write_record[\s\S]*rb_scheduler_master_pop_record' "${FILE}"; then
	echo "master record must be peeked, accepted by UART, then popped" >&2
	exit 1
fi
grep -q 'rb_scheduler_next_action' "${FILE}"
grep -q 'rb_scheduler_uart_available' "${FILE}"
for token in sync_pair_count sync_tracker_wait_count sync_tracker_error_count \
             sync_filter_update_count sync_filter_error_count \
             sync_tx_build_count sync_tx_capture_count sync_tx_failure_count \
             sync_tx_previous_count sync_last_tx_sequence sync_last_tx_tick \
             sync_last_previous_master_tick sync_last_sequence sync_last_error \
	     sync_relock_count sync_pps_reset_error_count sync_last_pps_reset_error \
	     slave_active slave_node_id action_error_count last_action \
             last_action_error last_action_error_action; do
	grep -q "${token}" "${ROOT}/src/bridge_runtime.h"
	grep -q "${token}" "${FILE}"
	grep -q "${token}" "${ROOT}/src/status.c"
done
# The loop must block with a bounded timeout (k_sleep or a k_msgq_get/k_sem_take
# with a K_USEC/K_MSEC timeout) rather than spin; radio/UART callbacks should
# wake it early via a k_msgq_put, and the timeout is only a fallback.
grep -Eq 'k_sleep|K_USEC\(|K_MSEC\(' "${FILE}"
grep -q 'set_wake_callback' "${FILE}"
if grep -Eq 'k_mutex_(lock|unlock)|k_malloc|k_free' "${FILE}"; then
	echo "runtime must not use blocking mutexes or heap" >&2
	exit 1
fi
if awk '/_isr|_callback/ { fn=1 } fn && /LOG_/ { exit 1 }' "${FILE}"; then
	:
fi
echo "Bridge runtime static check: PASS"
