#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "link_scheduler_core.h"

static struct rb_scheduler_core slave;

static void init_slave(void)
{
	const struct rb_scheduler_config config = {
		.master = false,
		.node_id = 2u,
		.group_id = 41u,
		.master_session = 1u,
		.slave_session = 0x12345678u,
		.sync_interval_us = 100000u,
		.lease_timeout_us = 100000u,
	};

	rb_scheduler_init(&slave, &config);
}

static void deliver_wire(uint8_t pipe, const uint8_t *wire, size_t len,
			 uint64_t now_us)
{
	const struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = pipe,
		.address_tick = now_us,
		.wire = wire,
		.wire_len = len,
	};

	rb_scheduler_on_radio_event(&slave, &event, now_us);
}

static void deliver_sync(uint32_t group_id, uint32_t master_session,
			 uint64_t now_us)
{
	struct rb_sync_frame sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC, master_session, 0u),
		.group_id = group_id,
		.master_id = 1u,
		.sync_sequence = 1u,
		.next_pps_master_tick = 1000000u,
		.sync_interval_us = 100000u,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_sync_encode(&sync, wire, sizeof(wire), &len));
	deliver_wire(0u, wire, len, now_us);
}

static void deliver_poll(uint8_t pipe, uint32_t master_session,
			 uint32_t known_slave_session,
			 uint32_t ack_sequence, uint64_t now_us)
{
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, master_session, 0u),
		.known_slave_session = known_slave_session,
		.uplink_ack_sequence = ack_sequence,
		.next_credit_bytes = RB_ACK_UPLINK_PAYLOAD_MAX,
		.poll_sequence = 1u,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	deliver_wire(pipe, wire, len, now_us);
}

static struct rb_ack_uplink take_ack(bool expected_replace, uint64_t now_us)
{
	struct rb_scheduler_action action;
	struct rb_ack_uplink ack;

	zassert_ok(rb_scheduler_next_action(&slave, now_us, &action));
	zassert_equal(action.type, RB_ACTION_QUEUE_ACK);
	zassert_equal(action.pipe, 2u);
	zassert_equal(action.replace_ack, expected_replace);
	zassert_ok(rb_ack_uplink_decode(action.wire, action.wire_len, &ack));
	return ack;
}

static void deliver_downlink(uint32_t master_session, uint32_t sequence,
			     const uint8_t *payload, size_t payload_len,
			     uint64_t now_us)
{
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_downlink_encode(master_session, sequence, payload, payload_len,
				      wire, sizeof(wire), &len));
	deliver_wire(0u, wire, len, now_us);
}

ZTEST(slave_scheduler, test_sync_queues_replacement_ack_on_fixed_pipe)
{
	struct rb_ack_uplink ack;

	init_slave();
	deliver_sync(41u, 9u, 1000u);
	ack = take_ack(true, 1000u);
	zassert_equal(ack.common.master_session, 9u);
	zassert_equal(ack.common.source_node, 2u);
	zassert_equal(ack.slave_session, 0x12345678u);
	zassert_equal(ack.payload_len, 0u);
}

ZTEST(slave_scheduler, test_first_sync_replaces_any_preloaded_data_with_empty_ack)
{
	const uint8_t payload[] = {0xa5u, 0x5au};
	struct rb_ack_uplink ack;

	init_slave();
	zassert_equal(rb_scheduler_uart_write(&slave, payload, sizeof(payload), 900u),
		      sizeof(payload));
	deliver_sync(41u, 9u, 1000u);
	ack = take_ack(true, 1000u);
	zassert_equal(ack.uplink_sequence, 0u);
	zassert_equal(ack.payload_len, 0u);
	zassert_equal(slave.slave_uplink_count, sizeof(payload));
}

ZTEST(slave_scheduler, test_group_mismatch_changes_no_session_or_ack_state)
{
	struct rb_scheduler_action action;

	init_slave();
	deliver_sync(42u, 9u, 1000u);
	zassert_equal(rb_scheduler_session(&slave), 1u);
	zassert_equal(rb_scheduler_next_action(&slave, 1000u, &action), -EAGAIN);
}

ZTEST(slave_scheduler, test_poll_marks_link_active_and_queues_next_ack)
{
	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	deliver_poll(2u, 9u, 0u, 0u, 2000u);
	zassert_true(slave.slave_active);
	(void)take_ack(false, 2000u);
}

ZTEST(slave_scheduler, test_uplink_repeats_until_session_and_sequence_are_acked)
{
	const uint8_t payload[] = {0xa5u, 0x5au};
	struct rb_ack_uplink first;
	struct rb_ack_uplink repeated;

	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	zassert_equal(rb_scheduler_uart_write(&slave, payload, sizeof(payload), 1100u),
		      sizeof(payload));
	deliver_poll(2u, 9u, 0u, 0u, 1200u);
	first = take_ack(false, 1200u);
	zassert_not_equal(first.uplink_sequence, 0u);
	zassert_mem_equal(first.payload, payload, sizeof(payload));
	deliver_poll(2u, 9u, 0x99999999u, first.uplink_sequence, 1300u);
	repeated = take_ack(false, 1300u);
	zassert_equal(repeated.uplink_sequence, first.uplink_sequence);
	deliver_poll(2u, 9u, 0x12345678u, first.uplink_sequence, 1400u);
	repeated = take_ack(false, 1400u);
	zassert_equal(repeated.uplink_sequence, 0u);
	zassert_equal(repeated.payload_len, 0u);
}

ZTEST(slave_scheduler, test_lease_expiry_retains_queued_uart_data)
{
	const uint8_t payload[] = {1u, 2u, 3u};
	struct rb_scheduler_action action;

	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	zassert_equal(rb_scheduler_uart_write(&slave, payload, sizeof(payload), 1100u),
		      sizeof(payload));
	deliver_poll(2u, 9u, 0u, 0u, 1200u);
	(void)take_ack(false, 1200u);
	zassert_equal(rb_scheduler_next_action(&slave, 101200u, &action), -EAGAIN);
	zassert_false(slave.slave_active);
	zassert_equal(slave.slave_uplink_count, sizeof(payload));
}

ZTEST(slave_scheduler, test_downlink_gap_delivers_later_record)
{
	uint8_t output[2];

	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	deliver_downlink(9u, 1u, (const uint8_t *)"a", 1u, 1100u);
	deliver_downlink(9u, 3u, (const uint8_t *)"c", 1u, 1200u);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, output, sizeof(output)), 2u);
	zassert_mem_equal(output, "ac", 2u);
	zassert_equal(rb_scheduler_downlink_gap_count(&slave), 1u);
}

ZTEST(slave_scheduler, test_downlink_duplicate_is_not_delivered_twice)
{
	uint8_t output;

	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	deliver_downlink(9u, 1u, (const uint8_t *)"a", 1u, 1100u);
	deliver_downlink(9u, 1u, (const uint8_t *)"a", 1u, 1200u);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, &output, 1u), 1u);
	zassert_equal(rb_scheduler_slave_read_uart(&slave, &output, 1u), 0u);
	zassert_equal(rb_scheduler_downlink_duplicate_count(&slave), 1u);
}

ZTEST(slave_scheduler, test_wrong_poll_pipe_and_session_are_rejected)
{
	init_slave();
	deliver_sync(41u, 9u, 1000u);
	(void)take_ack(true, 1000u);
	deliver_poll(1u, 9u, 0u, 0u, 1100u);
	deliver_poll(2u, 8u, 0u, 0u, 1200u);
	zassert_false(slave.slave_active);
	zassert_equal(rb_scheduler_invalid_node_count(&slave), 1u);
	zassert_equal(rb_scheduler_invalid_session_count(&slave), 1u);
}

ZTEST_SUITE(slave_scheduler, NULL, NULL, NULL, NULL, NULL);
