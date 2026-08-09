#ifndef WIRELESS_TIME_SYNC_H_
#define WIRELESS_TIME_SYNC_H_

#include <stdint.h>

#include "link_protocol.h"
#include "sync_filter.h"
#include "sync_tracker.h"

struct rb_wireless_time_sync {
	uint32_t session;
	uint32_t next_sequence;
	uint32_t sync_interval_us;
	uint64_t next_pps_master_tick;
	struct rb_sync_tracker tracker;
};

void wireless_time_sync_init(struct rb_wireless_time_sync *sync,
			     uint32_t session, uint32_t sync_interval_us,
			     uint64_t next_pps_master_tick);
int wireless_time_sync_master_build(struct rb_wireless_time_sync *sync,
				    uint64_t next_pps_tick,
				    struct rb_sync_discovery *frame);
void wireless_time_sync_master_tx_captured(struct rb_wireless_time_sync *sync,
					   uint32_t sequence, uint64_t tick);
int wireless_time_sync_slave_receive(struct rb_wireless_time_sync *sync,
				     const struct rb_sync_discovery *frame,
				     uint64_t local_address_tick,
				     struct sync_observation *observation);

#endif /* WIRELESS_TIME_SYNC_H_ */
