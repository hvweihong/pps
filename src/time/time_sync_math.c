#include "time_sync_math.h"

uint64_t time_sync_next_period_tick(uint64_t tick, uint32_t period_us)
{
	return ((tick / period_us) + 1u) * period_us;
}

uint64_t time_sync_next_epoch_tick_after(uint64_t epoch_tick,
					 uint32_t period_us,
					 uint64_t after_tick)
{
	uint64_t periods;

	if (after_tick < epoch_tick) {
		return epoch_tick;
	}

	periods = ((after_tick - epoch_tick) / period_us) + 1u;
	return epoch_tick + (periods * period_us);
}

uint64_t time_sync_select_pps_target(uint64_t candidate_tick,
				     uint32_t period_us,
				     uint64_t now_tick,
				     uint32_t keep_pending_window_us,
				     uint64_t scheduled_tick,
				     uint32_t min_arm_ahead_us)
{
	uint64_t min_target = now_tick + min_arm_ahead_us;

	if (scheduled_tick == candidate_tick &&
	    candidate_tick > now_tick &&
	    candidate_tick - now_tick <= keep_pending_window_us) {
		return candidate_tick;
	}

	if (candidate_tick <= min_target) {
		return time_sync_next_epoch_tick_after(candidate_tick, period_us,
						       min_target);
	}

	return candidate_tick;
}
