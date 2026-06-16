#include "status.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "pps_output.h"
#include "radio_sync.h"

LOG_MODULE_REGISTER(time_sync_status, LOG_LEVEL_INF);

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
		"busy=%u tx_to=%u tx_time=%u tx_err=%d tx_ref=%u rx_ref=%u "
		"ts_block=%u ts_cancel=%u ts_over=%u",
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
		radio->tx_timeout_errors,
		radio->tx_timing_errors,
		radio->last_tx_error_us,
		radio->last_tx_ref_age_us,
		radio->last_rx_ref_age_us,
		radio->timeslot_blocked,
		radio->timeslot_cancelled,
		radio->timeslot_overstayed);
}
