#ifndef TIME_SYNC_MATH_H
#define TIME_SYNC_MATH_H

#include <stdint.h>

uint64_t time_sync_next_period_tick(uint64_t tick, uint32_t period_us);
uint64_t time_sync_next_epoch_tick_after(uint64_t epoch_tick,
					 uint32_t period_us,
					 uint64_t after_tick);
uint64_t time_sync_select_pps_target(uint64_t candidate_tick,
				     uint32_t period_us,
				     uint64_t now_tick,
				     uint32_t keep_pending_window_us,
				     uint64_t scheduled_tick,
				     uint32_t min_arm_ahead_us);

#endif
