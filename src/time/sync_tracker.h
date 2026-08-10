#ifndef SYNC_TRACKER_H_
#define SYNC_TRACKER_H_

#include <stdbool.h>
#include <stdint.h>

#include "sync_filter.h"

struct rb_sync_tracker {
	uint32_t master_session;
	bool local_valid;
	uint32_t local_sequence;
	uint64_t local_address_tick;
	bool paired_valid;
	uint32_t last_paired_sequence;
};

void rb_sync_tracker_init(struct rb_sync_tracker *tracker);
void rb_sync_tracker_reset_session(struct rb_sync_tracker *tracker,
				   uint32_t master_session);
void rb_sync_tracker_record_local(struct rb_sync_tracker *tracker,
				  uint32_t sequence, uint64_t address_tick);
int rb_sync_tracker_pair(struct rb_sync_tracker *tracker,
			 uint32_t current_sequence, uint32_t previous_sequence,
			 uint64_t previous_master_address_tick,
			 uint64_t next_pps_master_tick,
			 struct sync_observation *observation);

#endif /* SYNC_TRACKER_H_ */
