#include <zephyr/ztest.h>

#include "bridge_runtime.h"

ZTEST(bridge_runtime_policy, test_mismatched_group_changes_no_sync_state)
{
	struct rb_bridge_stats stats = {
		.invalid_group_packets = 4u,
		.sync_rx_count = 5u,
		.sync_last_sequence = 6u,
		.sync_relock_count = 7u,
		.utc_seconds = 1700000000,
		.utc_quality = RB_TIME_LOCKED,
	};

	zassert_false(rb_bridge_runtime_accept_sync_group(1u, 2u, &stats));
	zassert_equal(stats.invalid_group_packets, 5u);
	zassert_equal(stats.sync_rx_count, 5u);
	zassert_equal(stats.sync_last_sequence, 6u);
	zassert_equal(stats.sync_relock_count, 7u);
	zassert_equal(stats.utc_seconds, 1700000000);
	zassert_equal(stats.utc_quality, RB_TIME_LOCKED);
}

ZTEST(bridge_runtime_policy, test_matching_group_changes_no_counters)
{
	struct rb_bridge_stats stats = {
		.invalid_group_packets = 4u,
		.sync_rx_count = 5u,
	};

	zassert_true(rb_bridge_runtime_accept_sync_group(1u, 1u, &stats));
	zassert_equal(stats.invalid_group_packets, 4u);
	zassert_equal(stats.sync_rx_count, 5u);
}

ZTEST_SUITE(bridge_runtime_policy, NULL, NULL, NULL, NULL, NULL);
