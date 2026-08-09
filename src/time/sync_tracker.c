#include "sync_tracker.h"

#include <errno.h>
#include <stddef.h>

void rb_sync_tracker_init(struct rb_sync_tracker *tracker)
{
	if (tracker == NULL) {
		return;
	}
	*tracker = (struct rb_sync_tracker){0};
}

void rb_sync_tracker_reset_session(struct rb_sync_tracker *tracker,
				   uint32_t master_session)
{
	if (tracker == NULL) {
		return;
	}
	tracker->master_session = master_session;
	tracker->local_valid = false;
	tracker->local_sequence = 0;
	tracker->local_address_tick = 0;
	tracker->paired_valid = false;
	tracker->last_paired_sequence = 0;
}

void rb_sync_tracker_record_local(struct rb_sync_tracker *tracker,
				  uint32_t sequence, uint64_t address_tick)
{
	if (tracker == NULL || tracker->master_session == 0) {
		return;
	}
	tracker->local_valid = true;
	tracker->local_sequence = sequence;
	tracker->local_address_tick = address_tick;
}

int rb_sync_tracker_pair(struct rb_sync_tracker *tracker,
			 uint32_t current_sequence, uint32_t previous_sequence,
			 uint64_t previous_master_address_tick,
			 uint64_t next_pps_master_tick,
			 struct sync_observation *observation)
{
	if (tracker == NULL || observation == NULL ||
	    tracker->master_session == 0) {
		return -EINVAL;
	}
	if (tracker->paired_valid &&
	    tracker->last_paired_sequence == current_sequence) {
		return -EALREADY;
	}
	if (current_sequence != previous_sequence + 1 || !tracker->local_valid ||
	    tracker->local_sequence != previous_sequence) {
		return -EAGAIN;
	}

	*observation = (struct sync_observation){
		.master_tick = previous_master_address_tick,
		.local_tick = tracker->local_address_tick,
		.next_pps_master_tick = next_pps_master_tick,
	};
	tracker->local_valid = false;
	tracker->paired_valid = true;
	tracker->last_paired_sequence = current_sequence;
	return 0;
}
