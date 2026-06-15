#ifndef SYNC_FILTER_H
#define SYNC_FILTER_H

#include <errno.h>
#include <stdint.h>

enum sync_filter_state {
	SYNC_FILTER_UNLOCKED,
	SYNC_FILTER_ACQUIRING,
	SYNC_FILTER_LOCKED,
	SYNC_FILTER_HOLDOVER,
};

struct sync_filter_config {
	uint32_t sync_interval_us;
	uint8_t lock_beacons;
	uint32_t outlier_threshold_us;
	uint8_t missed_holdover;
	uint32_t unlock_timeout_us;
};

struct sync_observation {
	uint64_t master_tick;
	uint64_t local_tick;
	uint64_t next_pps_master_tick;
};

struct sync_filter {
	struct sync_filter_config cfg;
	enum sync_filter_state state;
	uint8_t valid_count;
	uint32_t missed_count;
	uint32_t age_us;
	int64_t offset_us;
	int32_t drift_ppm;
	uint64_t last_master_tick;
	uint64_t last_local_tick;
	uint64_t next_pps_master_tick;
};

struct sync_filter_config sync_filter_default_config(void);
void sync_filter_init(struct sync_filter *filter,
		      const struct sync_filter_config *cfg);
int sync_filter_update(struct sync_filter *filter,
		       const struct sync_observation *obs);
void sync_filter_note_missed(struct sync_filter *filter, uint32_t missed_count);
void sync_filter_age(struct sync_filter *filter, uint32_t elapsed_us);
enum sync_filter_state sync_filter_state(const struct sync_filter *filter);
int64_t sync_filter_offset_us(const struct sync_filter *filter);
int32_t sync_filter_drift_ppm(const struct sync_filter *filter);
int sync_filter_master_to_local(const struct sync_filter *filter,
				uint64_t master_tick, uint64_t *local_tick);

#endif
