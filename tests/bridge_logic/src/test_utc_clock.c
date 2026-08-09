#include <errno.h>
#include <limits.h>

#include <zephyr/ztest.h>

#include "utc_clock.h"

static uint64_t reset_tick;
static int reset_count;
static int reset_result;

static int phase_reset(void *context, uint64_t target_tick)
{
	(void)context;
	reset_tick = target_tick;
	reset_count++;
	return reset_result;
}

static void setup_clock(struct rb_utc_clock *clock)
{
	struct rb_utc_clock_config config = {
		.external_mode = true,
		.loss_timeout_us = 3000000,
		.phase_reset = phase_reset,
	};
	reset_tick = 0;
	reset_count = 0;
	reset_result = 0;
	rb_utc_clock_init(clock, &config);
}

static void add_pair(struct rb_utc_clock *clock, uint64_t tick, int64_t second)
{
	rb_utc_clock_tick(clock, tick);
	zassert_ok(rb_utc_clock_note_pair(clock, tick, second));
}

static void lock_clock(struct rb_utc_clock *clock)
{
	add_pair(clock, 1000000, 100);
	add_pair(clock, 2000000, 101);
	add_pair(clock, 3000000, 102);
	zassert_equal(rb_utc_clock_state(clock), RB_UTC_LOCKED);
}

ZTEST(utc_clock, test_local_mode_is_utc_invalid)
{
	struct rb_utc_clock clock;
	struct rb_utc_clock_config config = {.external_mode = false, .loss_timeout_us = 3000000};
	struct rb_utc_publication publication;
	rb_utc_clock_init(&clock, &config);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_LOCAL);
	zassert_ok(rb_utc_clock_publication(&clock, 0, &publication));
	zassert_false(publication.valid);
	zassert_equal(publication.quality, RB_TIME_UTC_INVALID);
	zassert_equal(rb_utc_clock_note_pair(&clock, 1000000, 100), -EACCES);
}

ZTEST(utc_clock, test_external_cold_boot_is_acquiring_but_utc_invalid)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_ok(rb_utc_clock_publication(&clock, 0, &publication));
	zassert_false(publication.valid);
	zassert_equal(publication.quality, RB_TIME_UTC_INVALID);
}

ZTEST(utc_clock, test_first_external_pair_hard_realigns)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	add_pair(&clock, 1000000, 100);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_equal(reset_count, 1);
	zassert_equal(reset_tick, 2000000);
}

ZTEST(utc_clock, test_three_pairs_enter_locked)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	add_pair(&clock, 1000000, 100);
	add_pair(&clock, 2000000, 101);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	add_pair(&clock, 3000000, 102);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_LOCKED);
	zassert_ok(rb_utc_clock_publication(&clock, 3000000, &publication));
	zassert_true(publication.valid);
	zassert_equal(publication.quality, RB_TIME_LOCKED);
	zassert_equal(publication.next_pps_utc_seconds, 103);
}

ZTEST(utc_clock, test_utc_second_gap_restarts_acquisition)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	add_pair(&clock, 1000000, 100);
	rb_utc_clock_tick(&clock, 2000000);
	zassert_ok(rb_utc_clock_note_pair(&clock, 2000000, 102));
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_equal(clock.valid_pairs, 1);
	zassert_equal(reset_count, 2);
}

ZTEST(utc_clock, test_discontinuous_pair_restarts_acquisition)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	add_pair(&clock, 1000000, 100);
	add_pair(&clock, 3000000, 102);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_equal(reset_count, 2);
	add_pair(&clock, 4000000, 103);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	add_pair(&clock, 5000000, 104);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_LOCKED);
}

ZTEST(utc_clock, test_failed_discontinuous_reset_preserves_old_baseline)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	add_pair(&clock, 1000000, 100);
	reset_result = -EIO;
	rb_utc_clock_tick(&clock, 3000000);
	zassert_equal(rb_utc_clock_note_pair(&clock, 3000000, 102), -EIO);
	zassert_equal(clock.last_pair_tick, 1000000);
	zassert_equal(clock.last_pair_second, 100);
	zassert_equal(clock.valid_pairs, 1);
	reset_result = 0;
	add_pair(&clock, 4000000, 103);
	add_pair(&clock, 5000000, 104);
	add_pair(&clock, 6000000, 105);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_LOCKED);
}

ZTEST(utc_clock, test_phase_reset_failure_does_not_accept_pair)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	reset_result = -EIO;
	rb_utc_clock_tick(&clock, 1000000);
	zassert_equal(rb_utc_clock_note_pair(&clock, 1000000, 100), -EIO);
	zassert_equal(reset_count, 1);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_equal(rb_utc_clock_note_pair(&clock, 1000000, 100), -EIO);
}

ZTEST(utc_clock, test_missing_pps_or_nmea_for_three_seconds_enters_holdover)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	lock_clock(&clock);
	rb_utc_clock_tick(&clock, 6000000);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_HOLDOVER);
}

ZTEST(utc_clock, test_holdover_increments_last_utc)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	lock_clock(&clock);
	rb_utc_clock_tick(&clock, 6000000);
	zassert_ok(rb_utc_clock_publication(&clock, 6000000, &publication));
	zassert_true(publication.valid);
	zassert_equal(publication.quality, RB_TIME_HOLDOVER);
	zassert_equal(publication.next_pps_utc_seconds, 106);
}

ZTEST(utc_clock, test_publication_projects_to_its_now_tick)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	lock_clock(&clock);
	zassert_ok(rb_utc_clock_publication(&clock, 4000000, &publication));
	zassert_true(publication.valid);
	zassert_equal(publication.quality, RB_TIME_LOCKED);
	zassert_equal(publication.next_pps_utc_seconds, 104);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_LOCKED);
}

ZTEST(utc_clock, test_recovery_skips_edge_under_500ms)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	lock_clock(&clock);
	rb_utc_clock_tick(&clock, 6000000);
	zassert_equal(rb_utc_clock_note_pair(&clock, 5100000, 104), 0);
	zassert_equal(rb_utc_clock_state(&clock), RB_UTC_ACQUIRING);
	zassert_equal(reset_tick, 7100000);
	zassert_ok(rb_utc_clock_publication(&clock, 6000000, &publication));
	zassert_equal(publication.next_pps_utc_seconds, 106);
}

ZTEST(utc_clock, test_pair_after_900ms_is_rejected)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	rb_utc_clock_tick(&clock, 1000001);
	zassert_equal(rb_utc_clock_note_pair(&clock, 100000, 100), -ERANGE);
	rb_utc_clock_tick(&clock, 1000000);
	zassert_equal(rb_utc_clock_note_pair(&clock, 1000000, 100), 0);
}

ZTEST(utc_clock, test_overflowing_pair_is_rejected_without_state_change)
{
	struct rb_utc_clock clock;
	setup_clock(&clock);
	rb_utc_clock_tick(&clock, UINT64_MAX);
	zassert_equal(rb_utc_clock_note_pair(&clock, UINT64_MAX - 1, 100), -ERANGE);
	zassert_false(clock.have_pair);
	rb_utc_clock_tick(&clock, 1000000);
	zassert_equal(rb_utc_clock_note_pair(&clock, 1000000, INT64_MAX), -ERANGE);
	zassert_false(clock.have_pair);
}

ZTEST(utc_clock, test_backward_ticks_do_not_regress_clock_or_publication)
{
	struct rb_utc_clock clock;
	struct rb_utc_publication publication;
	setup_clock(&clock);
	rb_utc_clock_tick(&clock, 2000000);
	rb_utc_clock_tick(&clock, 1000000);
	zassert_equal(clock.now_tick, 2000000);
	zassert_equal(rb_utc_clock_publication(&clock, 1999999, &publication), -ERANGE);
}

ZTEST_SUITE(utc_clock, NULL, NULL, NULL, NULL, NULL);
