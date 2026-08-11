#include "wireless_time_sync.h"

#include <errno.h>

void wireless_time_sync_init(struct rb_wireless_time_sync *sync,
			     uint32_t session, uint32_t sync_interval_us,
			     uint64_t next_pps_master_tick,
			     uint32_t radio_delay_us)
{
	if (sync == NULL) {
		return;
	}
	*sync = (struct rb_wireless_time_sync){
		.session = session,
		.next_sequence = 1,
		.sync_interval_us = sync_interval_us,
		.radio_delay_us = radio_delay_us,
		.next_pps_master_tick = next_pps_master_tick,
	};
	rb_sync_tracker_init(&sync->tracker);
	rb_sync_tracker_reset_session(&sync->tracker, session);
}

int wireless_time_sync_master_build(struct rb_wireless_time_sync *sync,
				    uint64_t next_pps_tick,
				    struct rb_sync_frame *frame)
{
	if (sync == NULL || frame == NULL || sync->session == 0u) {
		return -EINVAL;
	}
	*frame = (struct rb_sync_frame){
		.common = RB_COMMON_INIT(RB_FRAME_SYNC, sync->session, 0u),
		.sync_sequence = sync->next_sequence,
		.next_pps_master_tick = next_pps_tick,
		.sync_interval_us = sync->sync_interval_us,
	};
	if (sync->tracker.local_valid) {
		frame->previous_master_address_tick = sync->tracker.local_address_tick;
	}
	return 0;
}

void wireless_time_sync_master_tx_captured(struct rb_wireless_time_sync *sync,
					   uint32_t sequence, uint64_t tick)
{
	if (sync == NULL || sequence == 0u) {
		return;
	}
	rb_sync_tracker_record_local(&sync->tracker, sequence, tick);
	sync->next_sequence = sequence + 1u;
	if (sync->next_sequence == 0u) {
		sync->next_sequence = 1u;
	}
}

int wireless_time_sync_slave_receive(struct rb_wireless_time_sync *sync,
				     const struct rb_sync_frame *frame,
				     uint64_t local_address_tick,
				     struct sync_observation *observation)
{
	if (sync == NULL || frame == NULL || observation == NULL ||
	    frame->common.master_session == 0u || frame->sync_sequence == 0u) {
		return -EINVAL;
	}
	if (sync->session != frame->common.master_session) {
		sync->session = frame->common.master_session;
		rb_sync_tracker_reset_session(&sync->tracker, sync->session);
	}
	if (frame->previous_master_address_tick == 0u) {
		rb_sync_tracker_record_local(&sync->tracker, frame->sync_sequence,
					     local_address_tick);
		return -EAGAIN;
	}
	int err = rb_sync_tracker_pair(&sync->tracker, frame->sync_sequence,
				       frame->sync_sequence - 1u,
				       frame->previous_master_address_tick,
				       frame->next_pps_master_tick, observation);
	if (err == 0) {
		observation->master_tick += sync->radio_delay_us;
	}
	rb_sync_tracker_record_local(&sync->tracker, frame->sync_sequence,
				     local_address_tick);
	return err;
}
