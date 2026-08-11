#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "sync_tracker.h"

ZTEST(sync_tracker, test_second_frame_pairs_previous_address_capture)
{
	struct rb_sync_tracker tracker;
	struct sync_observation obs;

	rb_sync_tracker_init(&tracker);
	rb_sync_tracker_reset_session(&tracker, 0x1234);
	rb_sync_tracker_record_local(&tracker, 10, 500100);
	zassert_ok(rb_sync_tracker_pair(&tracker, 11, 10, 500000, 1000000,
					&obs));
	zassert_equal(obs.master_tick, 500000);
	zassert_equal(obs.local_tick, 500100);
	zassert_equal(obs.next_pps_master_tick, 1000000);
}

ZTEST(sync_tracker, test_first_frame_and_mismatch_wait_for_complete_pair)
{
	struct rb_sync_tracker tracker;
	struct sync_observation obs = {0};

	rb_sync_tracker_init(&tracker);
	rb_sync_tracker_reset_session(&tracker, 1);
	zassert_equal(rb_sync_tracker_pair(&tracker, 1, 0, 100, 1000, &obs),
		      -EAGAIN);
	rb_sync_tracker_record_local(&tracker, 4, 400);
	zassert_equal(rb_sync_tracker_pair(&tracker, 6, 5, 500, 1000, &obs),
		      -EAGAIN);
	zassert_true(tracker.local_valid);
	zassert_equal(tracker.local_sequence, 4);
}

ZTEST(sync_tracker, test_duplicate_pair_is_rejected)
{
	struct rb_sync_tracker tracker;
	struct sync_observation obs;

	rb_sync_tracker_init(&tracker);
	rb_sync_tracker_reset_session(&tracker, 1);
	rb_sync_tracker_record_local(&tracker, 10, 1000);
	zassert_ok(rb_sync_tracker_pair(&tracker, 11, 10, 900, 2000, &obs));
	zassert_equal(rb_sync_tracker_pair(&tracker, 11, 10, 900, 2000, &obs),
		      -EALREADY);
}

ZTEST(sync_tracker, test_session_reset_discards_old_capture)
{
	struct rb_sync_tracker tracker;
	struct sync_observation obs;

	rb_sync_tracker_init(&tracker);
	rb_sync_tracker_reset_session(&tracker, 1);
	rb_sync_tracker_record_local(&tracker, 10, 1000);
	rb_sync_tracker_reset_session(&tracker, 2);
	zassert_equal(rb_sync_tracker_pair(&tracker, 11, 10, 900, 2000, &obs),
		      -EAGAIN);
	zassert_false(tracker.local_valid);
}

ZTEST(sync_tracker, test_sync_sequence_wrap_pairs_normally)
{
	struct rb_sync_tracker tracker;
	struct sync_observation obs;

	rb_sync_tracker_init(&tracker);
	rb_sync_tracker_reset_session(&tracker, 1);
	rb_sync_tracker_record_local(&tracker, UINT32_MAX, 1000);
	zassert_ok(rb_sync_tracker_pair(&tracker, 0, UINT32_MAX, 900, 2000, &obs));
	zassert_equal(obs.local_tick, 1000);
}

ZTEST_SUITE(sync_tracker, NULL, NULL, NULL, NULL, NULL);
