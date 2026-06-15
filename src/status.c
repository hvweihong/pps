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
		"age=%lluus next_pps=%lluus pps=%u late=%u tx=%u rx=%u "
		"crc_err=%u decode_err=%u busy=%u",
		snapshot->role,
		snapshot->seq,
		status_filter_state_name(snapshot->filter_state),
		snapshot->offset_us,
		snapshot->drift_ppm,
		snapshot->missed_beacons,
		snapshot->last_beacon_age_us,
		snapshot->next_pps_tick,
		pps->pulses,
		pps->late_schedules,
		radio->tx_packets,
		radio->rx_packets,
		radio->rx_crc_errors,
		radio->rx_decode_errors,
		radio->busy_errors);
}
