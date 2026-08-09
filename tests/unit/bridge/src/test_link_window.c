#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "link_window.h"

ZTEST(link_window, test_tx_window_selects_oldest_missing_repair)
{
	struct rb_tx_window window;
	struct rb_packet_slot slots[RB_LINK_WINDOW_SIZE];

	rb_tx_window_init(&window, 7, slots, ARRAY_SIZE(slots));
	zassert_ok(rb_tx_window_store(&window, 100, (uint8_t[]){1}, 1));
	zassert_ok(rb_tx_window_store(&window, 101, (uint8_t[]){2}, 1));
	zassert_ok(rb_tx_window_store(&window, 102, (uint8_t[]){3}, 1));
	rb_tx_window_apply_ack(&window, 99, BIT64(1));

	zassert_not_null(rb_tx_window_next_repair(&window));
	zassert_equal(rb_tx_window_next_repair(&window)->sequence, 100);
	zassert_false(rb_tx_window_contains(&window, 101));
	zassert_true(rb_tx_window_contains(&window, 102));
}

ZTEST(link_window, test_tx_cumulative_ack_and_eviction)
{
	struct rb_tx_window window;
	struct rb_packet_slot slots[RB_LINK_WINDOW_SIZE];
	uint32_t evicted;

	rb_tx_window_init(&window, 1, slots, ARRAY_SIZE(slots));
	for (uint32_t sequence = 10; sequence < 74; sequence++) {
		zassert_ok(rb_tx_window_store(&window, sequence, (uint8_t *)&sequence, 1));
	}
	zassert_equal(window.count, RB_LINK_WINDOW_SIZE);
	zassert_ok(rb_tx_window_store(&window, 74, (uint8_t[]){0xaa}, 1));
	zassert_false(rb_tx_window_contains(&window, 10));
	zassert_true(rb_tx_window_take_evicted(&window, &evicted));
	zassert_equal(evicted, 10);
	zassert_false(rb_tx_window_take_evicted(&window, &evicted));

	rb_tx_window_apply_ack(&window, 72, 0);
	zassert_false(rb_tx_window_contains(&window, 72));
	zassert_true(rb_tx_window_contains(&window, 73));
	zassert_true(rb_tx_window_contains(&window, 74));
}

ZTEST(link_window, test_rx_reorders_and_rejects_duplicate)
{
	struct rb_rx_window window;
	struct rb_packet_slot slots[RB_LINK_WINDOW_SIZE];
	uint8_t out[2];
	size_t out_len;
	uint32_t sequence;

	rb_rx_window_init(&window, 3, 10, slots, ARRAY_SIZE(slots));
	zassert_ok(rb_rx_window_insert(&window, 11, (uint8_t[]){0xbb}, 1));
	zassert_equal(rb_rx_window_pop(&window, &sequence, out, sizeof(out), &out_len),
		      -EAGAIN);
	zassert_equal(window.ack_base, 9);
	zassert_equal(window.ack_bitmap, BIT64(1));
	zassert_ok(rb_rx_window_insert(&window, 10, (uint8_t[]){0xaa}, 1));
	zassert_equal(window.ack_base, 11);
	zassert_equal(window.ack_bitmap, 0);
	zassert_equal(rb_rx_window_insert(&window, 10, (uint8_t[]){0xaa}, 1),
		      -EALREADY);
	zassert_ok(rb_rx_window_pop(&window, &sequence, out, sizeof(out), &out_len));
	zassert_equal(sequence, 10);
	zassert_equal(out_len, 1);
	zassert_equal(out[0], 0xaa);
	zassert_ok(rb_rx_window_pop(&window, &sequence, out, sizeof(out), &out_len));
	zassert_equal(sequence, 11);
	zassert_equal(out[0], 0xbb);
}

ZTEST(link_window, test_rx_skip_to_discards_unrecoverable_gap)
{
	struct rb_rx_window window;
	struct rb_packet_slot slots[RB_LINK_WINDOW_SIZE];
	uint8_t out;
	size_t out_len;
	uint32_t sequence;

	rb_rx_window_init(&window, 4, 10, slots, ARRAY_SIZE(slots));
	zassert_ok(rb_rx_window_insert(&window, 11, (uint8_t[]){0x11}, 1));
	rb_rx_window_skip_to(&window, 20);
	zassert_equal(window.ack_base, 19);
	zassert_equal(window.ack_bitmap, 0);
	zassert_false(rb_rx_window_contains(&window, 11));
	zassert_ok(rb_rx_window_insert(&window, 20, (uint8_t[]){0x20}, 1));
	zassert_equal(window.ack_base, 20);
	zassert_ok(rb_rx_window_pop(&window, &sequence, &out, sizeof(out), &out_len));
	zassert_equal(sequence, 20);
	zassert_equal(out, 0x20);
}

ZTEST(link_window, test_sequence_wrap_preserves_order_and_ack_bitmap)
{
	struct rb_rx_window window;
	struct rb_packet_slot slots[RB_LINK_WINDOW_SIZE];
	uint8_t out;
	size_t out_len;
	uint32_t sequence;

	rb_rx_window_init(&window, 5, UINT32_MAX - 1, slots, ARRAY_SIZE(slots));
	zassert_ok(rb_rx_window_insert(&window, 0, (uint8_t[]){0}, 1));
	zassert_equal(window.ack_base, UINT32_MAX - 2);
	zassert_equal(window.ack_bitmap, BIT64(2));
	zassert_ok(rb_rx_window_insert(&window, UINT32_MAX - 1,
				    (uint8_t[]){0xfe}, 1));
	zassert_ok(rb_rx_window_insert(&window, UINT32_MAX,
				    (uint8_t[]){0xff}, 1));
	zassert_equal(window.ack_base, 0);
	zassert_ok(rb_rx_window_insert(&window, 1, (uint8_t[]){1}, 1));
	zassert_equal(window.ack_base, 1);

	for (uint32_t expected = UINT32_MAX - 1;; expected++) {
		zassert_ok(rb_rx_window_pop(&window, &sequence, &out, sizeof(out),
					  &out_len));
		zassert_equal(sequence, expected);
		if (expected == 1) {
			break;
		}
	}
}

ZTEST_SUITE(link_window, NULL, NULL, NULL, NULL, NULL);
