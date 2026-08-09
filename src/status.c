#include "status.h"

#include <zephyr/logging/log.h>

#include "bridge_runtime.h"
#include "pps_output.h"
#include "pps_input.h"
#include "time_uart.h"
#include "uart_bridge.h"

LOG_MODULE_REGISTER(star_bridge_status, LOG_LEVEL_INF);

void status_log_bridge(void)
{
	struct rb_bridge_stats bridge;
	const struct rb_uart_stats *uart = uart_bridge_stats_get();
	const struct pps_output_stats *pps = pps_output_stats_get();
	const struct rb_time_uart_stats *time_uart = time_uart_stats_get();

	bridge_runtime_stats_get(&bridge);
	LOG_INF("bridge_status uart_rx_bytes=%llu uart_tx_bytes=%llu "
		"uart_rx_drop_bytes=%llu uart_tx_drop_bytes=%llu "
		"uart_rx_restart_errors=%u",
		(unsigned long long)uart->rx_bytes,
		(unsigned long long)uart->tx_bytes,
		(unsigned long long)uart->rx_drop_bytes,
		(unsigned long long)uart->tx_drop_bytes, uart->rx_restart_errors);
	LOG_INF("time_uart rx_bytes=%llu rx_drop_bytes=%llu lines=%u "
		"overlong_line_drops=%u output_line_drops=%u "
		"rx_restart_errors=%u rx_buffer_errors=%u rx_stopped_events=%u "
		"rx_stop_reason_mask=%u pps_input_count=%llu "
		"pps_input_drop_count=%llu",
		(unsigned long long)time_uart->rx_bytes,
		(unsigned long long)time_uart->rx_drop_bytes,
		time_uart->lines_received, time_uart->overlong_line_drops,
		time_uart->output_line_drops, time_uart->rx_restart_errors,
		time_uart->rx_buffer_errors, time_uart->rx_stopped_events,
		time_uart->rx_stop_reason_mask,
		(unsigned long long)pps_input_count(),
		(unsigned long long)pps_input_drop_count());
	LOG_INF("bridge_radio radio_tx_packets=%llu radio_rx_packets=%llu "
		"radio_retry_count=%llu radio_retry_exhausted=%llu "
		"broadcast_packets=%llu poll_packets=%llu repair_packets=%llu",
		(unsigned long long)bridge.radio_tx_packets,
		(unsigned long long)bridge.radio_rx_packets,
		(unsigned long long)bridge.radio_retry_count,
		(unsigned long long)bridge.radio_retry_exhausted,
		(unsigned long long)bridge.broadcast_packets,
		(unsigned long long)bridge.poll_packets,
		(unsigned long long)bridge.repair_packets);
	LOG_INF("bridge_link duplicate_packets=%llu invalid_session_packets=%llu "
		"radio_history_drop_packets=%llu unrecoverable_gap_count=%llu "
		"queue_drop_bytes=%llu "
		"active_count=%u suspect_count=%u slave_active=%u slave_node_id=%u "
		"discovery_hello_count=%llu action_error_count=%llu "
		"last_action=%u last_action_error=%d "
		"last_action_error_action=%u",
		(unsigned long long)bridge.duplicate_packets,
		(unsigned long long)bridge.invalid_session_packets,
		(unsigned long long)bridge.radio_history_drop_packets,
		(unsigned long long)bridge.unrecoverable_gap_count,
		(unsigned long long)bridge.queue_drop_bytes,
		bridge.active_count, bridge.suspect_count, bridge.slave_active,
		bridge.slave_node_id,
		(unsigned long long)bridge.discovery_hello_count,
		(unsigned long long)bridge.action_error_count,
		bridge.last_action, bridge.last_action_error,
		bridge.last_action_error_action);
	LOG_INF("bridge_sync sync_rx_count=%llu sync_missed_count=%llu "
		"sync_pair_count=%llu sync_filter_update_count=%llu "
		"sync_filter_error_count=%llu sync_state=%u "
		"sync_offset=%lld sync_jitter=%u",
		(unsigned long long)bridge.sync_rx_count,
		(unsigned long long)bridge.sync_missed_count,
		(unsigned long long)bridge.sync_pair_count,
		(unsigned long long)bridge.sync_filter_update_count,
		(unsigned long long)bridge.sync_filter_error_count,
		bridge.sync_state,
		(long long)bridge.sync_offset, bridge.sync_jitter);
	LOG_INF("bridge_sync_flow sync_tracker_wait_count=%llu "
		"sync_tracker_error_count=%llu sync_last_sequence=%u "
		"sync_last_previous_master_tick=%llu sync_last_error=%d "
		"sync_relock_count=%llu sync_pps_reset_error_count=%llu "
		"sync_last_pps_reset_error=%d",
		(unsigned long long)bridge.sync_tracker_wait_count,
		(unsigned long long)bridge.sync_tracker_error_count,
		bridge.sync_last_sequence,
		(unsigned long long)bridge.sync_last_previous_master_tick,
		bridge.sync_last_error,
		(unsigned long long)bridge.sync_relock_count,
		(unsigned long long)bridge.sync_pps_reset_error_count,
		bridge.sync_last_pps_reset_error);
	LOG_INF("bridge_sync_tx sync_tx_build_count=%llu "
		"sync_tx_capture_count=%llu sync_tx_failure_count=%llu "
		"sync_tx_previous_count=%llu sync_last_tx_sequence=%u "
		"sync_last_tx_tick=%llu",
		(unsigned long long)bridge.sync_tx_build_count,
		(unsigned long long)bridge.sync_tx_capture_count,
		(unsigned long long)bridge.sync_tx_failure_count,
		(unsigned long long)bridge.sync_tx_previous_count,
		bridge.sync_last_tx_sequence,
		(unsigned long long)bridge.sync_last_tx_tick);
	LOG_INF("bridge_pps pps_count=%u pps_last_tick=%llu "
		"scheduled_rise_tick=%llu phase_resets=%u late_schedules=%u "
		"phase_pending=%u",
		pps->pulses, (unsigned long long)pps->last_rise_tick,
		(unsigned long long)pps->scheduled_rise_tick, pps->phase_resets,
		pps->late_schedules, pps->phase_pending);
}
