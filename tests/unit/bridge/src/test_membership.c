#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "membership.h"

ZTEST(membership, test_first_three_assignments_fill_capacity_without_preemption)
{
	struct rb_membership membership;

	rb_membership_init(&membership, 0x1234, 100000);
	zassert_equal(rb_membership_begin_assign(&membership, 10, 1000, 1, 101), 1);
	zassert_ok(rb_membership_assign_acked(&membership, 1, 1010));
	zassert_equal(rb_membership_begin_assign(&membership, 20, 1020, 2, 102), 2);
	zassert_ok(rb_membership_assign_acked(&membership, 2, 1030));
	zassert_equal(rb_membership_begin_assign(&membership, 30, 1040, 3, 103), 3);
	zassert_ok(rb_membership_assign_acked(&membership, 3, 1050));

	zassert_equal(rb_membership_active_count(&membership), 3);
	zassert_equal(rb_membership_free_slots(&membership), 0);
	zassert_equal(rb_membership_begin_assign(&membership, 40, 1060, 1, 104),
		      -ENOSPC);
	zassert_equal(rb_membership_begin_assign(&membership, 20, 1070, 1, 200), 2);
	zassert_equal(rb_membership_active_count(&membership), 3);
	zassert_equal(rb_membership_peer(&membership, 2)->lease_id, 102);
}

ZTEST(membership, test_assignment_only_activates_after_hardware_ack)
{
	struct rb_membership membership;

	rb_membership_init(&membership, 1, 100000);
	zassert_equal(rb_membership_begin_assign(&membership, 77, 1000, 1, 0x55aa),
		      1);
	zassert_equal(rb_membership_active_count(&membership), 0);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_RESERVED);
	rb_membership_tick(&membership, 100999);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_RESERVED);
	rb_membership_tick(&membership, 101000);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_FREE);
}

ZTEST(membership, test_failed_transaction_enters_suspect_then_expires)
{
	struct rb_membership membership;

	rb_membership_init(&membership, 0x1234, 100000);
	rb_membership_begin_assign(&membership, 77, 1000, 1, 0x55aa);
	rb_membership_assign_acked(&membership, 1, 1010);

	rb_membership_note_failure(&membership, 1, 2000);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_SUSPECT);
	rb_membership_tick(&membership, 101999);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_SUSPECT);
	rb_membership_tick(&membership, 102000);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_FREE);
	zassert_equal(rb_membership_active_count(&membership), 0);
}

ZTEST(membership, test_success_recovers_suspect_without_changing_lease)
{
	struct rb_membership membership;

	rb_membership_init(&membership, 1, 100000);
	rb_membership_begin_assign(&membership, 77, 1000, 1, 9);
	rb_membership_assign_acked(&membership, 1, 1010);
	rb_membership_note_failure(&membership, 1, 2000);
	zassert_ok(rb_membership_note_success(&membership, 1, 50000));
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_ACTIVE);
	zassert_equal(rb_membership_peer(&membership, 1)->lease_id, 9);
	rb_membership_tick(&membership, 149999);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_ACTIVE);
	rb_membership_tick(&membership, 150000);
	zassert_equal(rb_membership_peer(&membership, 1)->state, RB_PEER_FREE);
}

ZTEST(membership, test_slave_lease_expires_without_valid_poll)
{
	struct rb_slave_lease lease;

	rb_slave_lease_init(&lease, 100000);
	zassert_ok(rb_slave_lease_assign(&lease, 0x11223344, 2, 0x55aa, 1000));
	zassert_true(rb_slave_lease_active(&lease));
	zassert_ok(rb_slave_lease_note_poll(&lease, 0x11223344, 2, 0x55aa, 50000));
	zassert_false(rb_slave_lease_tick(&lease, 149999));
	zassert_true(rb_slave_lease_tick(&lease, 150000));
	zassert_false(rb_slave_lease_active(&lease));
}

ZTEST(membership, test_time_rollback_does_not_expire_membership_or_lease)
{
	struct rb_membership membership;
	struct rb_slave_lease lease;

	rb_membership_init(&membership, 1, 100000);
	zassert_equal(rb_membership_begin_assign(&membership, 77, 1000, 1, 9), 1);
	rb_membership_tick(&membership, 500);
	zassert_equal(rb_membership_peer(&membership, 1)->state,
		      RB_PEER_RESERVED);
	zassert_ok(rb_membership_assign_acked(&membership, 1, 2000));
	rb_membership_tick(&membership, 1000);
	zassert_equal(rb_membership_peer(&membership, 1)->state,
		      RB_PEER_ACTIVE);

	rb_slave_lease_init(&lease, 100000);
	zassert_ok(rb_slave_lease_assign(&lease, 1, 1, 9, 2000));
	zassert_false(rb_slave_lease_tick(&lease, 1000));
	zassert_true(rb_slave_lease_active(&lease));
}

ZTEST_SUITE(membership, NULL, NULL, NULL, NULL, NULL);
