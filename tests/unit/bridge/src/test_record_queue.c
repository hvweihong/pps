#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "link_scheduler_core.h"
#include "record_queue.h"

ZTEST(record_queue, test_push_peek_pop_preserves_record_lengths)
{
	uint8_t storage[8];
	uint16_t lengths[3];
	struct rb_record_queue queue;
	const uint8_t expected[] = {0x10u, 0x11u};
	uint8_t output[3];
	size_t record_len;

	rb_record_queue_init(&queue, storage, sizeof(storage), lengths,
			     ARRAY_SIZE(lengths));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x10u, 0x11u}, 2u));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x20u}, 1u));
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 2u);
	zassert_mem_equal(output, expected, sizeof(expected));
	zassert_ok(rb_record_queue_pop(&queue));
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 1u);
	zassert_equal(output[0], 0x20u);
}

ZTEST(record_queue, test_full_queue_rejects_whole_new_record)
{
	uint8_t storage[3];
	uint16_t lengths[1];
	struct rb_record_queue queue;
	const uint8_t expected[] = {0x10u, 0x11u};
	uint8_t output[3];
	size_t record_len;

	rb_record_queue_init(&queue, storage, sizeof(storage), lengths,
			     ARRAY_SIZE(lengths));
	zassert_ok(rb_record_queue_push(&queue, (uint8_t[]){0x10u, 0x11u}, 2u));
	zassert_equal(rb_record_queue_push(&queue, (uint8_t[]){0x20u}, 1u),
		      -ENOSPC);
	zassert_ok(rb_record_queue_peek(&queue, output, sizeof(output), &record_len));
	zassert_equal(record_len, 2u);
	zassert_mem_equal(output, expected, sizeof(expected));
}

static void deliver_ack(struct rb_scheduler_core *core, uint8_t node_id,
			uint32_t slave_session, uint32_t sequence,
			const uint8_t *payload, size_t payload_len)
{
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7u, node_id),
		.slave_session = slave_session,
		.uplink_sequence = sequence,
		.payload = payload,
		.payload_len = payload_len,
	};
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = node_id,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len));
	event.wire = wire;
	event.wire_len = len;
	rb_scheduler_on_radio_event(core, &event, sequence);
}

ZTEST(record_queue, test_master_reads_complete_node_records_round_robin)
{
	const struct rb_scheduler_config config = {
		.master = true,
		.node_id = 0u,
		.group_id = 41u,
		.master_session = 7u,
		.local_device_id = 1u,
		.sync_interval_us = 100000u,
		.lease_timeout_us = 100000u,
	};
	struct rb_scheduler_core core;
	uint8_t output[2];
	uint8_t node_id;
	size_t record_len;

	rb_scheduler_init(&core, &config);
	deliver_ack(&core, 1u, 0x11u, 1u,
		    (uint8_t[]){0x10u, 0x11u}, 2u);
	deliver_ack(&core, 2u, 0x22u, 1u, (uint8_t[]){0x20u}, 1u);
	deliver_ack(&core, 1u, 0x11u, 2u, (uint8_t[]){0x12u}, 1u);

	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
					   sizeof(output), &record_len));
	zassert_equal(node_id, 1u);
	zassert_equal(record_len, 2u);
	zassert_ok(rb_scheduler_master_pop_record(&core, node_id));
	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
					   sizeof(output), &record_len));
	zassert_equal(node_id, 2u);
	zassert_equal(output[0], 0x20u);
	zassert_ok(rb_scheduler_master_pop_record(&core, node_id));
	zassert_ok(rb_scheduler_master_peek_record(&core, &node_id, output,
					   sizeof(output), &record_len));
	zassert_equal(node_id, 1u);
	zassert_equal(output[0], 0x12u);
}

ZTEST_SUITE(record_queue, NULL, NULL, NULL, NULL, NULL);
