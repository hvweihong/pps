#include <errno.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "fixed_peer_table.h"

ZTEST(fixed_peer_table, test_ack_activates_refreshes_and_changes_session)
{
	struct rb_fixed_peer_table table;

	rb_fixed_peer_table_init(&table, 100000u);
	zassert_equal(rb_fixed_peer_note_ack(&table, 2u, 0x11u, 1000u),
		      RB_FIXED_PEER_ACTIVATED);
	zassert_equal(rb_fixed_peer_note_ack(&table, 2u, 0x11u, 2000u),
		      RB_FIXED_PEER_REFRESHED);
	zassert_equal(rb_fixed_peer_note_ack(&table, 2u, 0x22u, 3000u),
		      RB_FIXED_PEER_SESSION_CHANGED);
	zassert_equal(rb_fixed_peer_active_mask(&table), BIT(1));
	zassert_equal(rb_fixed_peer_active_count(&table), 1u);
	zassert_true(rb_fixed_peer_expire(&table, 103000u));
	zassert_equal(rb_fixed_peer_active_count(&table), 0u);
}

ZTEST(fixed_peer_table, test_invalid_node_or_zero_session_is_rejected)
{
	struct rb_fixed_peer_table table;

	rb_fixed_peer_table_init(&table, 100000u);
	zassert_equal(rb_fixed_peer_note_ack(&table, 0u, 1u, 0u),
		      RB_FIXED_PEER_INVALID);
	zassert_equal(rb_fixed_peer_note_ack(&table, 4u, 1u, 0u),
		      RB_FIXED_PEER_INVALID);
	zassert_equal(rb_fixed_peer_note_ack(&table, 1u, 0u, 0u),
		      RB_FIXED_PEER_INVALID);
	zassert_equal(rb_fixed_peer_active_mask(&table), 0u);
}

ZTEST_SUITE(fixed_peer_table, NULL, NULL, NULL, NULL, NULL);
