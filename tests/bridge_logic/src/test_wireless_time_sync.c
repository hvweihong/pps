#include <zephyr/ztest.h>

#include "wireless_time_sync.h"

ZTEST(wireless_time_sync, test_master_first_frame_has_no_previous_capture)
{
	struct rb_wireless_time_sync sync;
	struct rb_sync_discovery frame;

	wireless_time_sync_init(&sync, 7, 100000, 1000000, 0);
	zassert_ok(wireless_time_sync_master_build(&sync, 1000000, &frame));
	zassert_equal(frame.sync_sequence, 1);
	zassert_equal(frame.previous_master_address_tick, 0);

	wireless_time_sync_master_tx_captured(&sync, 1, 1234);
	zassert_ok(wireless_time_sync_master_build(&sync, 1100000, &frame));
	zassert_equal(frame.previous_master_address_tick, 1234);
}

ZTEST(wireless_time_sync, test_slave_pairs_two_consecutive_sync_frames)
{
	struct rb_wireless_time_sync sync;
	struct rb_sync_discovery first;
	struct rb_sync_discovery second;
	struct sync_observation observation;

	wireless_time_sync_init(&sync, 0, 100000, 0, 17);
	first.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0);
	first.sync_sequence = 1;
	first.next_pps_master_tick = 1000000;
	second = first;
	second.sync_sequence = 2;
	second.previous_master_address_tick = 5000;
	zassert_equal(wireless_time_sync_slave_receive(&sync, &first, 7000,
							NULL), -EINVAL);
	zassert_equal(wireless_time_sync_slave_receive(&sync, &first, 7000,
							&observation), -EAGAIN);
	zassert_ok(wireless_time_sync_slave_receive(&sync, &second, 8000,
						    &observation));
	zassert_equal(observation.local_tick, 7000);
	zassert_equal(observation.master_tick, 5017);
}

ZTEST_SUITE(wireless_time_sync, NULL, NULL, NULL, NULL, NULL);
