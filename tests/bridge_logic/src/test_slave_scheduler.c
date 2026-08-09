#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "link_scheduler_core.h"

static struct rb_scheduler_core slave;
static struct rb_scheduler_config slave_config;

static void init_slave(void)
{
	slave_config = (struct rb_scheduler_config){
		.master = false,
		.group_id = 41,
		.master_session = 7,
		.device_id = 10,
		.sync_interval_us = 100000,
		.lease_timeout_us = 100000,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	rb_scheduler_init(&slave, &slave_config);
}

static void deliver(const uint8_t *wire, size_t len, uint64_t tick)
{
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.address_tick = tick,
		.wire = wire,
		.wire_len = len,
	};
	rb_scheduler_on_radio_event(&slave, &event, tick);
}

static void complete_tx(enum rb_radio_event_view_type type, uint64_t tick)
{
	struct rb_radio_event_view event = {
		.type = type,
	};

	rb_scheduler_on_radio_event(&slave, &event, tick);
}

ZTEST(slave_scheduler, test_matching_discovery_schedules_hashed_slot_hello)
{
	struct rb_sync_discovery sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0),
		.group_id = 41,
		.sync_sequence = 2,
		.discovery_nonce = 0xabc,
		.free_slots = 1,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_scheduler_action action;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	init_slave();
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &len));
	deliver(wire, len, 10000);
	zassert_equal(rb_scheduler_next_action(&slave, 10999, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&slave, 11000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_HELLO);
	zassert_equal(action.due_tick, 0);
	zassert_true(slave.transaction_in_flight);
	zassert_equal(rb_scheduler_next_action(&slave, 11001 + 8 * 500, &action),
		      -EAGAIN);
	complete_tx(RB_EVENT_VIEW_TX_SUCCESS, 11050);
	zassert_ok(rb_scheduler_next_action(&slave, 11001 + 8 * 500, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_ASSIGN_RX);

	/* A group mismatch never arms a response. */
	init_slave();
	sync.group_id = 42;
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &len));
	deliver(wire, len, 10000);
	zassert_equal(rb_scheduler_next_action(&slave, 20000, &action), -EAGAIN);
}

ZTEST(slave_scheduler, test_failed_hello_retries_after_global_backoff)
{
	struct rb_sync_discovery sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0),
		.group_id = 41,
		.sync_sequence = 2,
		.discovery_nonce = 0xabc,
		.free_slots = 1,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_scheduler_action action;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	init_slave();
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &len));
	deliver(wire, len, 10000u);
	zassert_ok(rb_scheduler_next_action(&slave, 11000u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_HELLO);
	rb_scheduler_action_failed(&slave, &action, 11000u);
	zassert_equal(rb_scheduler_next_action(&slave, 11999u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&slave, 12000u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_HELLO);
}

ZTEST(slave_scheduler, test_failed_assign_rx_retries_full_window)
{
	struct rb_scheduler_action action;

	init_slave();
	slave.slave_assign_rx_pending = true;
	slave.slave_assign_rx_deadline = 100u;
	zassert_ok(rb_scheduler_next_action(&slave, 100u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_ASSIGN_RX);
	rb_scheduler_action_failed(&slave, &action, 100u);
	zassert_false(slave.slave_assign_rx_active);
	zassert_equal(rb_scheduler_next_action(&slave, 1099u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&slave, 1100u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_ASSIGN_RX);
	zassert_equal(slave.slave_assign_rx_deadline,
		      1100u + slave.config.assignment_window_us);
}

ZTEST(slave_scheduler, test_failed_group_rx_retries_profile_action)
{
	struct rb_scheduler_action action;

	init_slave();
	slave.slave_group_rx_pending = true;
	zassert_ok(rb_scheduler_next_action(&slave, 100u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_GROUP_RX);
	rb_scheduler_action_failed(&slave, &action, 100u);
	zassert_equal(rb_scheduler_next_action(&slave, 1099u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&slave, 1100u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_GROUP_RX);
}

ZTEST(slave_scheduler, test_failed_ack_queue_retries_same_payload)
{
	struct rb_scheduler_action action;
	const uint8_t expected[] = {0xa5, 0x5a};

	init_slave();
	slave.slave_ack_pending = true;
	slave.slave_ack_wire_len = sizeof(expected);
	memcpy(slave.slave_ack_wire, expected, sizeof(expected));
	zassert_ok(rb_scheduler_next_action(&slave, 100u, &action));
	zassert_equal(action.type, RB_ACTION_QUEUE_ACK);
	rb_scheduler_action_failed(&slave, &action, 100u);
	zassert_equal(rb_scheduler_next_action(&slave, 1099u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&slave, 1100u, &action));
	zassert_equal(action.type, RB_ACTION_QUEUE_ACK);
	zassert_mem_equal(action.wire, expected, sizeof(expected));
}

ZTEST(slave_scheduler, test_assign_poll_ack_delay_and_ordered_downlink)
{
	struct rb_sync_discovery sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0),
		.group_id = 41,
		.sync_sequence = 2,
		.discovery_nonce = 0xabc,
		.free_slots = 1,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_assign assign = {
		.common = RB_COMMON_INIT(RB_FRAME_ASSIGN, 9, 0x55aa, 0),
		.target_device_id = 10,
		.node_id = 1,
		.pipe = 1,
		.downlink_epoch = 4,
		.uplink_epoch = 8,
		.max_payload = RB_PACKET_DATA_MAX,
		.link_window = 64,
		.retry_count = 3,
		.lease_timeout_us = 100000,
	};
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 9, 0x55aa, 0),
		.uplink_epoch = 8,
		.next_credit_bytes = 128,
		.poll_sequence = 3,
	};
	struct rb_scheduler_action action;
	struct rb_ack_uplink ack;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	uint8_t data_wire[RB_ESB_MAX_PAYLOAD];
	size_t len;
	const uint8_t first[] = {'a'};
	const uint8_t second[] = {'b'};
	uint8_t uart[2];

	init_slave();
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &len));
	deliver(wire, len, 10000);
	zassert_ok(rb_assign_encode(&assign, wire, sizeof(wire), &len));
	deliver(wire, len, 12000);
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 13000);
	zassert_ok(rb_scheduler_next_action(&slave, 13000, &action));
	zassert_equal(action.type, RB_ACTION_QUEUE_ACK);
	/* The ACK returned for this poll is the one prepared before this poll. */
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.uplink_sequence, 0u);

	zassert_ok(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 9, 0, 4, 2, second,
					 sizeof(second), data_wire, sizeof(data_wire), &len));
	deliver(data_wire, len, 14000);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, uart, sizeof(uart)), 0);
	zassert_ok(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 9, 0, 4, 1, first,
					 sizeof(first), data_wire, sizeof(data_wire), &len));
	deliver(data_wire, len, 15000);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, uart, sizeof(uart)), 2);
	zassert_equal(uart[0], 'a');
	zassert_equal(uart[1], 'b');
	deliver(data_wire, len, 16000);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, uart, sizeof(uart)), 0);
}

ZTEST(slave_scheduler, test_uplink_repeats_until_master_acknowledges_sequence)
{
	struct rb_assign assign = {
		.common = RB_COMMON_INIT(RB_FRAME_ASSIGN, 7, 0x55aa, 0),
		.target_device_id = 10,
		.node_id = 1,
		.pipe = 1,
		.downlink_epoch = 4,
		.uplink_epoch = 8,
		.max_payload = RB_PACKET_DATA_MAX,
		.link_window = 64,
		.retry_count = 3,
		.lease_timeout_us = 100000,
	};
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 7, 0x55aa, 0),
		.uplink_epoch = 8,
		.next_credit_bytes = 128,
		.poll_sequence = 1,
	};
	const uint8_t payload[] = {0xa5, 0x5a};
	struct rb_scheduler_action action;
	struct rb_ack_uplink ack;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	init_slave();
	zassert_ok(rb_assign_encode(&assign, wire, sizeof(wire), &len));
	deliver(wire, len, 1000);
	zassert_equal(rb_scheduler_uart_write(&slave, payload, sizeof(payload), 1001),
		      sizeof(payload));
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 2000);
	zassert_ok(rb_scheduler_next_action(&slave, 2000, &action));
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.uplink_sequence, 1u);
	zassert_mem_equal(ack.payload, payload, sizeof(payload));

	poll.poll_sequence++;
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 3000);
	zassert_ok(rb_scheduler_next_action(&slave, 3000, &action));
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.uplink_sequence, 1u);
	zassert_equal(ack.payload_len, sizeof(payload));
	zassert_mem_equal(ack.payload, payload, sizeof(payload));

	poll.poll_sequence++;
	poll.uplink_ack_base = 1u;
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 4000);
	zassert_ok(rb_scheduler_next_action(&slave, 4000, &action));
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.uplink_sequence, 0u);
	zassert_equal(ack.payload_len, 0u);
}

ZTEST(slave_scheduler, test_uplink_splits_at_ack_payload_capacity)
{
	struct rb_assign assign = {
		.common = RB_COMMON_INIT(RB_FRAME_ASSIGN, 7, 0x55aa, 0),
		.target_device_id = 10,
		.node_id = 1,
		.pipe = 1,
		.downlink_epoch = 4,
		.uplink_epoch = 8,
		.max_payload = RB_PACKET_DATA_MAX,
		.link_window = 64,
		.retry_count = 3,
		.lease_timeout_us = 100000,
	};
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 7, 0x55aa, 0),
		.uplink_epoch = 8,
		.next_credit_bytes = RB_PACKET_DATA_MAX,
		.poll_sequence = 1,
	};
	uint8_t payload[RB_PACKET_DATA_MAX];
	struct rb_scheduler_action action;
	struct rb_ack_uplink ack;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)i;
	}
	init_slave();
	zassert_ok(rb_assign_encode(&assign, wire, sizeof(wire), &len));
	deliver(wire, len, 1000);
	zassert_equal(rb_scheduler_uart_write(&slave, payload, sizeof(payload), 1001),
		      sizeof(payload));
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 2000);
	zassert_ok(rb_scheduler_next_action(&slave, 2000, &action));
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.payload_len,
		      RB_ESB_MAX_PAYLOAD - RB_ACK_UPLINK_HEADER_SIZE);
	zassert_mem_equal(ack.payload, payload, ack.payload_len);

	poll.poll_sequence++;
	poll.uplink_ack_base = ack.uplink_sequence;
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver(wire, len, 3000);
	zassert_ok(rb_scheduler_next_action(&slave, 3000, &action));
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	zassert_equal(ack.payload_len,
		      sizeof(payload) - (RB_ESB_MAX_PAYLOAD -
					 RB_ACK_UPLINK_HEADER_SIZE));
	zassert_mem_equal(ack.payload,
			  payload + RB_ESB_MAX_PAYLOAD - RB_ACK_UPLINK_HEADER_SIZE,
			  ack.payload_len);
}

ZTEST_SUITE(slave_scheduler, NULL, NULL, NULL, NULL, NULL);
