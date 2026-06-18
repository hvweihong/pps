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

	LOG_INF("sync_status: %s seq=%u state=%s offset=%lldus drift=%dppm "
		"missed=%u age=%lluus next_pps=%lluus ble_conn=%u",
		snapshot->role,
		snapshot->seq,
		status_filter_state_name(snapshot->filter_state),
		snapshot->offset_us,
		snapshot->drift_ppm,
		snapshot->missed_beacons,
		snapshot->last_beacon_age_us,
		snapshot->next_pps_tick,
		snapshot->ble_connected ? 1u : 0u);

	LOG_INF("pps_status: %s sched=%lluus last=%lluus epoch=%lluus "
		"pulses=%u phase=%u pending=%u late=%u",
		snapshot->role,
		pps->scheduled_rise_tick,
		pps->last_rise_tick,
		pps->epoch_tick,
		pps->pulses,
		pps->phase_resets,
		pps->phase_pending,
		pps->late_schedules);

	LOG_INF("radio_tx_status: %s tx=%u tx_seq=%u tx_err=%d tx_ref=%u "
		"tx_len=%u busy=%u tx_busy=%u tx_req=%u tx_block=%u "
		"tx_cancel=%u tx_to=%u tx_time=%u tx_late=%u",
		snapshot->role,
		radio->tx_packets,
		radio->tx_sequence,
		radio->last_tx_error_us,
		radio->last_tx_ref_age_us,
		radio->last_tx_packet_len,
		radio->busy_errors,
		radio->tx_api_busy_errors,
		radio->tx_mpsl_request_errors,
		radio->tx_mpsl_blocked,
		radio->tx_mpsl_cancelled,
		radio->tx_timeout_errors,
		radio->tx_timing_errors,
		radio->tx_prepare_late);

	LOG_INF("radio_rx_status: %s rx=%u crc_err=%u decode_err=%u "
		"rx_ref=%u freq=%u tx_addr=%u rx_addrs=0x%02x white=%u "
		"rx_addr=%u rx_end=%u rx_len=%u rx_crc_ok=%u "
		"rx_bad_len=%u rxen=%u rx_ready=%u rx_state=%u "
		"rx_mode=%s rx_win=%u rx_open=%u rx_skip=%u rx_late=%u",
		snapshot->role,
		radio->rx_packets,
		radio->rx_crc_errors,
		radio->rx_decode_errors,
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
		status_rx_mode_name(radio->rx_mode),
		radio->rx_window_length_us,
		radio->rx_windows,
		radio->rx_window_skips,
		radio->rx_window_late);

	LOG_INF("timeslot_status: %s ts_block=%u ts_cancel=%u ts_over=%u",
		snapshot->role,
		radio->timeslot_blocked,
		radio->timeslot_cancelled,
		radio->timeslot_overstayed);

	LOG_INF("ble_link: %s init=%u ready=%u started=%u conn=%u "
		"scan=%u adv=%u peer=%u notify=%u conn_count=%u "
		"peer_count=%u gate=%u start_attempts=%u last_err=%d",
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
		snapshot->ble.peer_count,
		snapshot->ble_gate_count,
		snapshot->ble.start_attempts,
		snapshot->ble.last_error);

	LOG_INF("ble_scan: %s seen=%u match=%u type_drop=%u filter_drop=%u "
		"conn_req=%u conn_fail=%u",
		snapshot->role,
		snapshot->ble.scan_seen,
		snapshot->ble.scan_match,
		snapshot->ble.scan_reject_type,
		snapshot->ble.scan_reject_filter,
		snapshot->ble.connect_attempts,
		snapshot->ble.connect_failures);

	LOG_INF("ble_gatt: %s tx=%u ccc=%u sub=%u sub_fail=%u "
		"notify_sub=%u rx=%u write=%u write_ok=%u write_fail=%u",
		snapshot->role,
		snapshot->ble.gatt_tx_found,
		snapshot->ble.gatt_tx_ccc_found,
		snapshot->ble.gatt_subscribe_attempts,
		snapshot->ble.gatt_subscribe_failures,
		snapshot->ble.gatt_notify_subscribed,
		snapshot->ble.gatt_rx_found,
		snapshot->ble.gatt_write_attempts,
		snapshot->ble.gatt_write_successes,
		snapshot->ble.gatt_write_failures);
}
