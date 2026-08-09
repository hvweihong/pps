#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "link_scheduler_core.h"
#include "record_queue.h"

ZTEST(record_queue, test_push_peek_pop_preserves_record_lengths)
{
	uint8_t storage[8];
	uint16_t lengths[3];
	struct rb_record_queue queue;
	const uint8_t expected[] = {0x10, 0x11};
	uint8_t output[3];
	size_t record_len;

	rb_record_queue_init(&queue, storage, sizeof(storage), lengths,
			     ARRAY_SIZE(lengths));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x10, 0x11}, 2));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x20}, 1));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x12}, 1));

	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 2u);
	zassert_mem_equal(output, expected, sizeof(expected));
	zassert_ok(rb_record_queue_pop(&queue));
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 1u);
	zassert_equal(output[0], 0x20);
	zassert_ok(rb_record_queue_pop(&queue));
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(output[0], 0x12);
}

ZTEST(record_queue, test_full_queue_rejects_whole_new_record)
{
	uint8_t storage[3];
	uint16_t lengths[1];
	struct rb_record_queue queue;
	const uint8_t expected[] = {0x10, 0x11};
	uint8_t output[3];
	size_t record_len;

	rb_record_queue_init(&queue, storage, sizeof(storage), lengths,
			     ARRAY_SIZE(lengths));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x10, 0x11}, 2));
	zassert_equal(rb_record_queue_push(&queue, (uint8_t[]){0x20, 0x21}, 2),
		      -ENOSPC);
	zassert_equal(rb_record_queue_push(&queue, (uint8_t[]){0x30}, 1),
		      -ENOSPC);
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 2u);
	zassert_mem_equal(output, expected, sizeof(expected));
}

static void deliver_ack(struct rb_scheduler_core *core, uint8_t node_id,
			uint16_t lease_id, uint32_t sequence,
			const uint8_t *payload, size_t payload_len)
{
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7, lease_id, node_id),
		.uplink_epoch = 1,
		.uplink_sequence = sequence,
		.payload = payload,
		.payload_len = payload_len,
	};
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = node_id,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(core, &event, sequence);
}

ZTEST(record_queue, test_round_robin_reads_one_node_record_at_a_time)
{
	struct rb_scheduler_config config = {
		.master = true,
		.group_id = 41,
		.master_session = 7,
		.device_id = 1,
		.sync_interval_us = 100000,
		.lease_timeout_us = 100000,
	};
	struct rb_scheduler_core core;
	const uint8_t expected[] = {0x10, 0x11};
	uint8_t output[2];
	uint8_t node_id;
	size_t record_len;

	rb_scheduler_init(&core, &config);
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_add_active_peer(&core, 2, 0x22, 2, 0));
	deliver_ack(&core, 1, 1, 1, (uint8_t[]){0x10, 0x11}, 2);
	deliver_ack(&core, 2, 2, 1, (uint8_t[]){0x20}, 1);
	deliver_ack(&core, 1, 1, 2, (uint8_t[]){0x12}, 1);

	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
						 sizeof(output), &record_len));
	zassert_equal(node_id, 1u);
	zassert_equal(record_len, 2u);
	zassert_mem_equal(output, expected, sizeof(expected));
	zassert_ok(rb_scheduler_master_pop_record(&core, node_id));
	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
						 sizeof(output), &record_len));
	zassert_equal(node_id, 2u);
	zassert_equal(record_len, 1u);
	zassert_equal(output[0], 0x20);
	zassert_ok(rb_scheduler_master_pop_record(&core, node_id));
	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
						 sizeof(output), &record_len));
	zassert_equal(node_id, 1u);
	zassert_equal(output[0], 0x12);
}

ZTEST_SUITE(record_queue, NULL, NULL, NULL, NULL, NULL);
