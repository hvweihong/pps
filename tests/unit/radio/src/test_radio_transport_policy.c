#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "radio_transport_policy.h"

ZTEST(radio_transport_policy, test_master_enables_group_and_all_node_pipes)
{
	uint8_t mask;

	zassert_ok(rb_radio_pipe_mask(true, 0u, &mask));
	zassert_equal(mask, BIT_MASK(4));
	zassert_equal(rb_radio_pipe_mask(true, 1u, &mask), -EINVAL);
}

ZTEST(radio_transport_policy, test_slave_enables_group_and_own_node_only)
{
	uint8_t mask;

	zassert_ok(rb_radio_pipe_mask(false, 1u, &mask));
	zassert_equal(mask, BIT(0) | BIT(1));
	zassert_ok(rb_radio_pipe_mask(false, 2u, &mask));
	zassert_equal(mask, BIT(0) | BIT(2));
	zassert_ok(rb_radio_pipe_mask(false, 3u, &mask));
	zassert_equal(mask, BIT(0) | BIT(3));
	zassert_equal(rb_radio_pipe_mask(false, 0u, &mask), -EINVAL);
	zassert_equal(rb_radio_pipe_mask(false, 4u, &mask), -EINVAL);
}

ZTEST(radio_transport_policy, test_null_mask_is_rejected)
{
	zassert_equal(rb_radio_pipe_mask(true, 0u, NULL), -EINVAL);
}

ZTEST_SUITE(radio_transport_policy, NULL, NULL, NULL, NULL, NULL);
