#include "status.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "pps_output.h"
#include "radio_sync.h"

LOG_MODULE_REGISTER(time_sync_status, LOG_LEVEL_INF);

static const char *status_rx_mode_name(enum radio_sync_rx_mode mode)
{
	switch (mode) {
	case RADIO_SYNC_RX_STOPPED:
		return "stopped";
	case RADIO_SYNC_RX_ACQUIRE:
		return "acquire";
	case RADIO_SYNC_RX_RECOVERY_WINDOW:
		return "recovery_window";
	case RADIO_SYNC_RX_LOCKED_WINDOW:
		return "locked_window";
	default:
		return "unknown";
	}
}

const char *status_filter_state_name(enum sync_filter_state state)
{
	switch (state) {
	case SYNC_FILTER_UNLOCKED:
		return "unlocked";
	case SYNC_FILTER_ACQUIRING:
		return "acquiring";
	case SYNC_FILTER_LOCKED:
		return "locked";
	case SYNC_FILTER_HOLDOVER:
		return "holdover";
	default:
		return "unknown";
	}
}

void status_log(const struct status_snapshot *snapshot)
{
	const struct radio_sync_stats *radio = radio_sync_stats_get();
	const struct pps_output_stats *pps = pps_output_stats_get();

	if (snapshot == NULL) {
		return;
	}

	LOG_INF("%s seq=%u state=%s offset=%lldus drift=%dppm missed=%u "
		"age=%lluus next_pps=%lluus pps_sched=%lluus "
		"pps_last=%lluus pps_epoch=%lluus pps=%u phase=%u "
		"pending=%u late=%u tx=%u rx=%u crc_err=%u decode_err=%u "
		"busy=%u tx_busy=%u tx_req=%u tx_block=%u tx_cancel=%u "
		"tx_to=%u tx_time=%u tx_late=%u tx_seq=%u "
		"tx_err=%d tx_ref=%u tx_len=%u rx_ref=%u "
		"freq=%u tx_addr=%u rx_addrs=0x%02x white=%u "
		"rx_addr=%u rx_end=%u rx_len=%u rx_crc_ok=%u "
		"rx_bad_len=%u rxen=%u rx_ready=%u rx_state=%u "
		"ts_block=%u ts_cancel=%u ts_over=%u rx_mode=%s "
		"rx_win=%u rx_open=%u rx_skip=%u rx_late=%u ble_conn=%u",
		snapshot->role,
		snapshot->seq,
		status_filter_state_name(snapshot->filter_state),
		snapshot->offset_us,
		snapshot->drift_ppm,
		snapshot->missed_beacons,
		snapshot->last_beacon_age_us,
		snapshot->next_pps_tick,
		pps->scheduled_rise_tick,
		pps->last_rise_tick,
		pps->epoch_tick,
		pps->pulses,
		pps->phase_resets,
		pps->phase_pending,
		pps->late_schedules,
		radio->tx_packets,
		radio->rx_packets,
		radio->rx_crc_errors,
		radio->rx_decode_errors,
		radio->busy_errors,
		radio->tx_api_busy_errors,
		radio->tx_mpsl_request_errors,
		radio->tx_mpsl_blocked,
		radio->tx_mpsl_cancelled,
		radio->tx_timeout_errors,
		radio->tx_timing_errors,
		radio->tx_prepare_late,
		radio->tx_sequence,
		radio->last_tx_error_us,
		radio->last_tx_ref_age_us,
		radio->last_tx_packet_len,
		radio->last_rx_ref_age_us,
		radio->radio_frequency_mhz,
		radio->tx_address,
		radio->rx_address_mask,
		radio->radio_datawhiteiv,
		radio->rx_address_events,
		radio->rx_end_events,
		radio->last_rx_packet_len,
		radio->last_rx_crc_ok ? 1u : 0u,
		radio->rx_bad_length_events,
		radio->rxen_events,
		radio->rx_ready_events,
		radio->last_rx_radio_state,
		radio->timeslot_blocked,
		radio->timeslot_cancelled,
		radio->timeslot_overstayed,
		status_rx_mode_name(radio->rx_mode),
		radio->rx_window_length_us,
		radio->rx_windows,
		radio->rx_window_skips,
		radio->rx_window_late,
		snapshot->ble_connected ? 1u : 0u);

	LOG_INF("ble_status: %s ble_init=%u ble_ready=%u ble_started=%u "
		"ble_conn=%u ble_scan=%u ble_adv=%u ble_peer=%u "
		"ble_notify=%u ble_cnt=%u ble_gate=%u "
		"ble_scan_seen=%u ble_scan_match=%u "
		"ble_scan_type_drop=%u ble_scan_filter_drop=%u "
		"ble_conn_req=%u ble_conn_fail=%u "
		"ble_start_attempts=%u ble_last_err=%d",
		snapshot->role,
		snapshot->ble.initialized ? 1u : 0u,
		snapshot->ble.bt_ready ? 1u : 0u,
		snapshot->ble.started ? 1u : 0u,
		snapshot->ble.connected ? 1u : 0u,
		snapshot->ble.scanning ? 1u : 0u,
		snapshot->ble.advertising ? 1u : 0u,
		snapshot->ble.peer ? 1u : 0u,
		snapshot->ble.notify_enabled ? 1u : 0u,
		snapshot->ble.connection_count,
		snapshot->ble_gate_count,
		snapshot->ble.scan_seen,
		snapshot->ble.scan_match,
		snapshot->ble.scan_reject_type,
		snapshot->ble.scan_reject_filter,
		snapshot->ble.connect_attempts,
		snapshot->ble.connect_failures,
		snapshot->ble.start_attempts,
		snapshot->ble.last_error);
}
