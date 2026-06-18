#ifndef STATUS_H
#define STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_time_sync.h"
#include "sync_filter.h"

struct status_snapshot {
	const char *role;
	uint16_t seq;
	enum sync_filter_state filter_state;
	int64_t offset_us;
	int32_t drift_ppm;
	uint32_t missed_beacons;
	uint64_t next_pps_tick;
	uint64_t last_beacon_age_us;
	uint32_t ble_gate_count;
	struct ble_time_sync_snapshot ble;
	bool ble_connected;
};

void status_log(const struct status_snapshot *snapshot);
const char *status_filter_state_name(enum sync_filter_state state);

#endif
