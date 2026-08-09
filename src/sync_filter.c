#include "sync_filter.h"

#include <limits.h>
#include <stddef.h>

static int tick_delta_i64(uint64_t newer, uint64_t older, int64_t *delta)
{
	uint64_t diff;

	if (delta == NULL) {
		return -EINVAL;
	}

	if (newer >= older) {
		diff = newer - older;
		if (diff > INT64_MAX) {
			return -ERANGE;
		}
		*delta = (int64_t)diff;
	} else {
		diff = older - newer;
		if (diff > INT64_MAX) {
			return -ERANGE;
		}
		*delta = -(int64_t)diff;
	}

	return 0;
}

struct sync_filter_config sync_filter_default_config(void)
{
	return (struct sync_filter_config) {
		.sync_interval_us = 50000,
		.lock_beacons = 10,
		.outlier_threshold_us = 200,
		.missed_holdover = 5,
		.unlock_timeout_us = 2000000,
	};
}

void sync_filter_init(struct sync_filter *filter,
			      const struct sync_filter_config *cfg)
{
	if (filter == NULL) {
		return;
	}

	if (cfg == NULL) {
		filter->cfg = sync_filter_default_config();
	} else {
		filter->cfg = *cfg;
	}

	filter->state = SYNC_FILTER_UNLOCKED;
	filter->valid_count = 0;
	filter->missed_count = 0;
	filter->age_us = 0;
	filter->offset_us = 0;
	filter->drift_ppm = 0;
	filter->last_master_tick = 0;
	filter->last_local_tick = 0;
	filter->next_pps_master_tick = 0;
}

void sync_filter_reset(struct sync_filter *filter)
{
	struct sync_filter_config config;

	if (filter == NULL) {
		return;
	}
	config = filter->cfg;
	sync_filter_init(filter, &config);
}

int sync_filter_update(struct sync_filter *filter,
		       const struct sync_observation *obs)
{
	int64_t observed_offset;
	int64_t phase_error;

	if (filter == NULL || obs == NULL) {
		return -EINVAL;
	}

	if (obs->next_pps_master_tick <= obs->master_tick) {
		return -ERANGE;
	}

	int ret = tick_delta_i64(obs->master_tick, obs->local_tick,
				 &observed_offset);
	if (ret != 0) {
		return ret;
	}

	if (filter->state == SYNC_FILTER_LOCKED) {
		phase_error = observed_offset - filter->offset_us;
		if (phase_error > (int64_t)filter->cfg.outlier_threshold_us ||
		    phase_error < -(int64_t)filter->cfg.outlier_threshold_us) {
			return -ERANGE;
		}

		if (filter->last_master_tick != 0 && filter->last_local_tick != 0) {
			int64_t dm;
			int64_t dl;

			ret = tick_delta_i64(obs->master_tick,
					     filter->last_master_tick, &dm);
			if (ret != 0) {
				return ret;
			}

			ret = tick_delta_i64(obs->local_tick,
					     filter->last_local_tick, &dl);
			if (ret != 0 || dm <= 0 || dl <= 0) {
				return ret != 0 ? ret : -ERANGE;
			}

			filter->drift_ppm =
				(int32_t)(((dm - dl) * 1000000LL) / dl);
		}

		filter->offset_us += phase_error / 8;
	} else {
		filter->offset_us = observed_offset;
		filter->valid_count++;
		filter->state = filter->valid_count >= filter->cfg.lock_beacons ?
			SYNC_FILTER_LOCKED : SYNC_FILTER_ACQUIRING;
	}

	filter->last_master_tick = obs->master_tick;
	filter->last_local_tick = obs->local_tick;
	filter->next_pps_master_tick = obs->next_pps_master_tick;
	filter->missed_count = 0;
	filter->age_us = 0;

	return 0;
}

void sync_filter_note_missed(struct sync_filter *filter, uint32_t missed_count)
{
	if (filter == NULL) {
		return;
	}

	filter->missed_count += missed_count;
	if (filter->state == SYNC_FILTER_LOCKED &&
	    filter->missed_count >= filter->cfg.missed_holdover) {
		filter->state = SYNC_FILTER_HOLDOVER;
	}
}

void sync_filter_age(struct sync_filter *filter, uint32_t elapsed_us)
{
	if (filter == NULL) {
		return;
	}

	filter->age_us += elapsed_us;
	if (filter->age_us > filter->cfg.unlock_timeout_us) {
		filter->state = SYNC_FILTER_UNLOCKED;
		filter->valid_count = 0;
	}
}

enum sync_filter_state sync_filter_state(const struct sync_filter *filter)
{
	return filter == NULL ? SYNC_FILTER_UNLOCKED : filter->state;
}

int64_t sync_filter_offset_us(const struct sync_filter *filter)
{
	return filter == NULL ? 0 : filter->offset_us;
}

int32_t sync_filter_drift_ppm(const struct sync_filter *filter)
{
	return filter == NULL ? 0 : filter->drift_ppm;
}

int sync_filter_master_to_local(const struct sync_filter *filter,
				uint64_t master_tick, uint64_t *local_tick)
{
	int64_t local;

	if (filter == NULL || local_tick == NULL) {
		return -EINVAL;
	}

	if (filter->state == SYNC_FILTER_UNLOCKED ||
	    filter->state == SYNC_FILTER_ACQUIRING) {
		return -EAGAIN;
	}

	local = (int64_t)master_tick - filter->offset_us;
	if (local < 0) {
		return -ERANGE;
	}

	*local_tick = (uint64_t)local;
	return 0;
}
