#include <zephyr/ztest.h>

#include "sync_filter.h"
#include "sync_packet.h"
#include "heartbeat_led.h"
#include "time_sync_math.h"

ZTEST(sync_packet, test_packet_round_trip)
{
	struct sync_beacon in = {
		.magic = SYNC_BEACON_MAGIC,
		.version = SYNC_BEACON_VERSION,
		.role = SYNC_BEACON_ROLE_MASTER,
		.seq = 42,
		.network_id = 0x12345678,
		.master_tx_tick = 1000000,
		.next_pps_master_tick = 2000000,
		.sync_interval_us = 50000,
		.status_flags = 0x5,
	};
	struct sync_beacon out;
	uint8_t buf[SYNC_BEACON_WIRE_SIZE];

	zassert_equal(sync_beacon_encode(&in, buf, sizeof(buf)), 0);
	zassert_equal(sync_beacon_decode(buf, sizeof(buf), 0x12345678, &out), 0);
	zassert_equal(out.magic, SYNC_BEACON_MAGIC);
	zassert_equal(out.version, SYNC_BEACON_VERSION);
	zassert_equal(out.role, SYNC_BEACON_ROLE_MASTER);
	zassert_equal(out.seq, 42);
	zassert_equal(out.network_id, 0x12345678);
	zassert_equal(out.master_tx_tick, 1000000);
	zassert_equal(out.next_pps_master_tick, 2000000);
	zassert_equal(out.sync_interval_us, 50000);
	zassert_equal(out.status_flags, 0x5);
}

ZTEST(sync_packet, test_packet_rejects_bad_crc)
{
	struct sync_beacon in = {
		.magic = SYNC_BEACON_MAGIC,
		.version = SYNC_BEACON_VERSION,
		.role = SYNC_BEACON_ROLE_MASTER,
		.seq = 1,
		.network_id = 0x12345678,
		.master_tx_tick = 10,
		.next_pps_master_tick = 1000000,
		.sync_interval_us = 50000,
	};
	struct sync_beacon out;
	uint8_t buf[SYNC_BEACON_WIRE_SIZE];

	zassert_equal(sync_beacon_encode(&in, buf, sizeof(buf)), 0);
	buf[7] ^= 0x80;

	zassert_equal(sync_beacon_decode(buf, sizeof(buf), 0x12345678, &out),
		      -EBADMSG);
}

ZTEST(sync_packet, test_packet_rejects_wrong_network)
{
	struct sync_beacon in = {
		.magic = SYNC_BEACON_MAGIC,
		.version = SYNC_BEACON_VERSION,
		.role = SYNC_BEACON_ROLE_MASTER,
		.seq = 1,
		.network_id = 0x12345678,
		.master_tx_tick = 10,
		.next_pps_master_tick = 1000000,
		.sync_interval_us = 50000,
	};
	struct sync_beacon out;
	uint8_t buf[SYNC_BEACON_WIRE_SIZE];

	zassert_equal(sync_beacon_encode(&in, buf, sizeof(buf)), 0);
	zassert_equal(sync_beacon_decode(buf, sizeof(buf), 0x87654321, &out),
		      -EACCES);
}

ZTEST(sync_filter, test_filter_locks_after_consecutive_valid_beacons)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 100000,
		.local_tick = 100500,
		.next_pps_master_tick = 1000000,
	};

	sync_filter_init(&filter, &cfg);

	for (int i = 0; i < cfg.lock_beacons - 1; i++) {
		obs.master_tick += cfg.sync_interval_us;
		obs.local_tick += cfg.sync_interval_us;
		sync_filter_update(&filter, &obs);
		zassert_not_equal(sync_filter_state(&filter), SYNC_FILTER_LOCKED);
	}

	obs.master_tick += cfg.sync_interval_us;
	obs.local_tick += cfg.sync_interval_us;
	sync_filter_update(&filter, &obs);

	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_LOCKED);
	zassert_within(sync_filter_offset_us(&filter), -500, 1);
}

ZTEST(sync_filter, test_filter_rejects_locked_outlier)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 100000,
		.local_tick = 100500,
		.next_pps_master_tick = 1000000,
	};

	sync_filter_init(&filter, &cfg);

	for (int i = 0; i < cfg.lock_beacons; i++) {
		obs.master_tick += cfg.sync_interval_us;
		obs.local_tick += cfg.sync_interval_us;
		sync_filter_update(&filter, &obs);
	}

	int64_t before = sync_filter_offset_us(&filter);
	obs.master_tick += cfg.sync_interval_us;
	obs.local_tick += cfg.sync_interval_us + cfg.outlier_threshold_us + 1000;
	zassert_equal(sync_filter_update(&filter, &obs), -ERANGE);
	zassert_equal(sync_filter_offset_us(&filter), before);
}

ZTEST(sync_filter, test_filter_rejects_master_tick_after_next_pps)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 3650868124415ULL,
		.local_tick = 1294860,
		.next_pps_master_tick = 147000000,
	};

	sync_filter_init(&filter, &cfg);

	zassert_equal(sync_filter_update(&filter, &obs), -ERANGE);
	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_UNLOCKED);
}

ZTEST(sync_filter, test_filter_computes_negative_offset_without_underflow)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 1000000,
		.local_tick = 1000500,
		.next_pps_master_tick = 2000000,
	};

	sync_filter_init(&filter, &cfg);

	zassert_equal(sync_filter_update(&filter, &obs), 0);
	zassert_equal(sync_filter_offset_us(&filter), -500);
}

ZTEST(sync_filter, test_filter_enters_holdover_and_unlocks)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 100000,
		.local_tick = 100500,
		.next_pps_master_tick = 1000000,
	};

	sync_filter_init(&filter, &cfg);

	for (int i = 0; i < cfg.lock_beacons; i++) {
		obs.master_tick += cfg.sync_interval_us;
		obs.local_tick += cfg.sync_interval_us;
		sync_filter_update(&filter, &obs);
	}

	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_LOCKED);
	sync_filter_note_missed(&filter, cfg.missed_holdover);
	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_HOLDOVER);
	sync_filter_age(&filter, cfg.unlock_timeout_us + 1);
	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_UNLOCKED);
}

ZTEST(sync_filter, test_filter_converts_master_pps_to_local_tick)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 100000,
		.local_tick = 100500,
		.next_pps_master_tick = 1000000,
	};
	uint64_t local_pps;

	sync_filter_init(&filter, &cfg);

	for (int i = 0; i < cfg.lock_beacons; i++) {
		obs.master_tick += cfg.sync_interval_us;
		obs.local_tick += cfg.sync_interval_us;
		sync_filter_update(&filter, &obs);
	}

	zassert_equal(sync_filter_master_to_local(&filter, 1000000, &local_pps), 0);
	zassert_equal(local_pps, 1000500);
}

ZTEST(sync_filter, test_filter_master_to_local_predicts_drift_at_next_pps)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 1000000,
		.local_tick = 1000000,
		.next_pps_master_tick = 2000000,
	};
	uint64_t local_pps;

	cfg.lock_beacons = 1;
	sync_filter_init(&filter, &cfg);
	zassert_ok(sync_filter_update(&filter, &obs));
	/* Master advances 100 us more than local over a 1 s interval. */
	obs.master_tick = 2000100;
	obs.local_tick = 2000000;
	obs.next_pps_master_tick = 3000100;
	zassert_ok(sync_filter_update(&filter, &obs));
	zassert_ok(sync_filter_master_to_local(&filter, 3000100, &local_pps));
	zassert_equal(local_pps, 2999900);
}

ZTEST(sync_filter, test_filter_reset_preserves_config_and_clears_lock)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct sync_observation obs = {
		.master_tick = 100000,
		.local_tick = 100500,
		.next_pps_master_tick = 1000000,
	};
	uint64_t local_pps;

	cfg.lock_beacons = 2;
	cfg.outlier_threshold_us = 321;
	sync_filter_init(&filter, &cfg);
	for (int i = 0; i < cfg.lock_beacons; i++) {
		obs.master_tick += cfg.sync_interval_us;
		obs.local_tick += cfg.sync_interval_us;
		zassert_ok(sync_filter_update(&filter, &obs));
	}
	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_LOCKED);

	sync_filter_reset(&filter);

	zassert_equal(sync_filter_state(&filter), SYNC_FILTER_UNLOCKED);
	zassert_equal(sync_filter_offset_us(&filter), 0);
	zassert_equal(sync_filter_drift_ppm(&filter), 0);
	zassert_equal(filter.cfg.lock_beacons, 2);
	zassert_equal(filter.cfg.outlier_threshold_us, 321);
	zassert_equal(sync_filter_master_to_local(&filter, 1000000, &local_pps),
		      -EAGAIN);
}

ZTEST(heartbeat_led, test_default_heartbeat_period_is_one_second)
{
	zassert_equal(HEARTBEAT_LED_DEFAULT_PERIOD_MS, 1000);
}

ZTEST(time_sync_math, test_next_period_tick_uses_next_boundary)
{
	zassert_equal(time_sync_next_period_tick(0, 1000000), 1000000);
	zassert_equal(time_sync_next_period_tick(1, 1000000), 1000000);
	zassert_equal(time_sync_next_period_tick(999999, 1000000), 1000000);
	zassert_equal(time_sync_next_period_tick(1000000, 1000000), 2000000);
}

ZTEST(time_sync_math, test_next_epoch_tick_after_keeps_periodic_phase)
{
	zassert_equal(time_sync_next_epoch_tick_after(1000000, 1000000, 1),
		      1000000);
	zassert_equal(time_sync_next_epoch_tick_after(1000000, 1000000, 1000000),
		      2000000);
	zassert_equal(time_sync_next_epoch_tick_after(1000000, 1000000, 3500000),
		      4000000);
	zassert_equal(time_sync_next_epoch_tick_after(1000100, 1000000, 3500000),
		      4000100);
}

ZTEST(time_sync_math, test_select_pps_target_keeps_pending_edge_when_close)
{
	uint64_t target = time_sync_select_pps_target(1000000, 1000000,
						     999500, 20000,
						     1000000, 1000);

	zassert_equal(target, 1000000);
}

ZTEST(time_sync_math, test_select_pps_target_advances_unscheduled_late_edge)
{
	uint64_t target = time_sync_select_pps_target(1000000, 1000000,
						     999500, 20000, 0,
						     1000);

	zassert_equal(target, 2000000);
}

ZTEST_SUITE(sync_packet, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(sync_filter, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(heartbeat_led, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(time_sync_math, NULL, NULL, NULL, NULL, NULL);
