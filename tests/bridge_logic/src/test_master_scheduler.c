#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "link_scheduler_core.h"

static struct rb_scheduler_core core;
static void complete_master_tx(uint64_t now_us);

static void init_master(void)
{
	struct rb_scheduler_config config = {
		.master = true,
		.group_id = 1,
		.master_session = 7,
		.device_id = 0x100,
		.sync_interval_us = 100000,
		.aggregation_timeout_us = 1000,
		.max_idle_poll_us = 5000,
		.lease_timeout_us = 100000,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	rb_scheduler_init(&core, &config);
}

ZTEST(master_scheduler, test_sync_publication_is_injected_into_each_frame)
{
	struct rb_scheduler_action action;
	struct rb_sync_discovery frame;

	init_master();
	rb_scheduler_set_sync_deadline(&core, 100000u);
	zassert_ok(rb_scheduler_next_action(&core, 100000u, &action));
	zassert_ok(rb_sync_discovery_decode(action.wire, action.wire_len, &frame));
	zassert_equal(frame.next_pps_utc_seconds, 0);
	zassert_equal(frame.time_quality, RB_TIME_UTC_INVALID);

	zassert_ok(rb_scheduler_set_time_publication(&core, 1700000000,
							 RB_TIME_LOCKED));
	complete_master_tx(100000u);
	rb_scheduler_set_sync_deadline(&core, 200000u);
	zassert_ok(rb_scheduler_next_action(&core, 200000u, &action));
	zassert_ok(rb_sync_discovery_decode(action.wire, action.wire_len, &frame));
	zassert_equal(frame.next_pps_utc_seconds, 1700000000);
	zassert_equal(frame.time_quality, RB_TIME_LOCKED);
}

static void complete_master_tx(uint64_t now_us)
{
	const struct rb_radio_event_view tx_success = {
		.type = RB_EVENT_VIEW_TX_SUCCESS,
	};

	rb_scheduler_on_radio_event(&core, &tx_success, now_us);
}

ZTEST(master_scheduler, test_active_only_polling_and_round_robin)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_equal(rb_scheduler_next_action(&core, 0, &action), -EAGAIN);
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 1));
	zassert_ok(rb_scheduler_next_action(&core, 1, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 1);
	complete_master_tx(1);
	zassert_ok(rb_scheduler_add_active_peer(&core, 2, 0x22, 2, 2));
	zassert_ok(rb_scheduler_add_active_peer(&core, 3, 0x33, 3, 3));
	zassert_equal(core.membership.peers[2].state, RB_PEER_ACTIVE);
	zassert_equal(core.peer[2].poll_interval_us, 0);
	zassert_equal(core.peer[2].next_poll_due_us, 3);
	zassert_ok(rb_scheduler_next_action(&core, 2, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 2);
	complete_master_tx(2);
	zassert_ok(rb_scheduler_next_action(&core, 3, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 3);
	complete_master_tx(3);
	zassert_ok(rb_scheduler_next_action(&core, 4, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 1);
}

ZTEST(master_scheduler, test_poll_waits_for_radio_tx_completion)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_next_action(&core, 0, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_true(core.transaction_in_flight);
	zassert_equal(rb_scheduler_next_action(&core, 0, &action), -EAGAIN);
	complete_master_tx(1);
	zassert_ok(rb_scheduler_next_action(&core, 1, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 1);
}

ZTEST(master_scheduler, test_failed_poll_clears_gate_and_retries_after_backoff)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_next_action(&core, 0, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_true(core.transaction_in_flight);

	/* A transport submission failure has no TX event to clear the gate. */
	rb_scheduler_action_failed(&core, &action, 0);
	zassert_false(core.transaction_in_flight);
	zassert_equal(rb_scheduler_next_action(&core, 0, &action), -EAGAIN);
	zassert_equal(rb_scheduler_next_action(&core,
					      RB_SCHEDULER_DEFAULT_IDLE_POLL_US - 1u,
					      &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core,
					   RB_SCHEDULER_DEFAULT_IDLE_POLL_US,
					   &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
}

ZTEST(master_scheduler, test_failed_assign_releases_reserved_membership)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 0,
	};
	struct rb_hello hello = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 7, 0, 0),
		.device_id = 0x30,
		.discovery_nonce = 0x44,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	zassert_ok(rb_hello_encode(&hello, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 10);
	zassert_ok(rb_scheduler_next_action(&core, 10, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(core.membership.peers[0].state, RB_PEER_RESERVED);
	zassert_equal(core.free_slots, RB_SCHEDULER_MAX_PEERS - 1u);

	rb_scheduler_action_failed(&core, &action, 10);
	zassert_false(core.transaction_in_flight);
	zassert_equal(core.membership.peers[0].state, RB_PEER_FREE);
	zassert_equal(core.free_slots, RB_SCHEDULER_MAX_PEERS);
	zassert_equal(rb_scheduler_next_action(&core, 10, &action), -EAGAIN);
	zassert_equal(rb_scheduler_next_action(
		&core, 10u + RB_SCHEDULER_DEFAULT_IDLE_POLL_US - 1u, &action),
		-EAGAIN);
	zassert_ok(rb_scheduler_next_action(
		&core, 10u + RB_SCHEDULER_DEFAULT_IDLE_POLL_US, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
}

ZTEST(master_scheduler, test_broadcast_has_no_ack_and_no_history)
{
	struct rb_scheduler_action action;
	struct rb_data_frame frame;
	const uint8_t payload[] = {0x12, 0x34, 0x56};

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_equal(rb_scheduler_uart_write(&core, payload, sizeof(payload), 1u),
		      sizeof(payload));
	zassert_ok(rb_scheduler_next_action(&core, 1001u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);
	zassert_true(action.no_ack);
	zassert_ok(rb_data_decode(action.wire, action.wire_len, &frame));
	zassert_equal(frame.sequence, 1u);
	zassert_mem_equal(frame.payload, payload, sizeof(payload));

	rb_scheduler_action_failed(&core, &action, 1001u);
	zassert_equal(rb_scheduler_next_action(&core, 2000u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 2001u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_false(action.no_ack);
	zassert_equal(core.next_downlink_sequence, 2u);
}

ZTEST(master_scheduler, test_failed_discovery_rx_retries_profile_action)
{
	struct rb_scheduler_action action;

	init_master();
	rb_scheduler_set_free_slots(&core, 1u);
	rb_scheduler_set_sync_deadline(&core, 100000u);
	zassert_ok(rb_scheduler_next_action(&core, 100000u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC_DISCOVERY);
	complete_master_tx(100000u);
	zassert_ok(rb_scheduler_next_action(&core, 100000u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);

	rb_scheduler_action_failed(&core, &action, 100000u);
	zassert_equal(rb_scheduler_next_action(&core, 100999u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 101000u, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);
}

ZTEST(master_scheduler, test_broadcast_waits_for_radio_tx_completion)
{
	struct rb_scheduler_action action;
	const uint8_t payload = 0x55;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_queue_downlink(&core, &payload, sizeof(payload)));
	zassert_ok(rb_scheduler_next_action(&core, 0, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);
	zassert_true(core.transaction_in_flight);
	zassert_equal(rb_scheduler_next_action(&core, 0, &action), -EAGAIN);
}

ZTEST(master_scheduler, test_broadcast_precedes_poll)
{
	struct rb_scheduler_action action;
	const uint8_t payload[] = {0, 1, 2, 3};

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 1));
	zassert_ok(rb_scheduler_add_active_peer(&core, 2, 0x22, 2, 2));
	zassert_ok(rb_scheduler_add_active_peer(&core, 3, 0x33, 3, 3));
	zassert_ok(rb_scheduler_queue_downlink(&core, payload, sizeof(payload)));
	zassert_ok(rb_scheduler_next_action(&core, 10, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);
	zassert_equal(action.node_id, 0);
	complete_master_tx(10);
	zassert_ok(rb_scheduler_next_action(&core, 11, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 1);
	complete_master_tx(11);
	zassert_ok(rb_scheduler_next_action(&core, 12, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 2);
	complete_master_tx(12);
	zassert_ok(rb_scheduler_next_action(&core, 13, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 3);
}

ZTEST(master_scheduler, test_sync_discovery_priority_and_free_slot_gate)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view tx_success = {
		.type = RB_EVENT_VIEW_TX_SUCCESS,
	};
	const uint8_t byte = 0x55;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_queue_downlink(&core, &byte, sizeof(byte)));
	rb_scheduler_set_free_slots(&core, 1);
	rb_scheduler_set_sync_deadline(&core, 100000);
	zassert_ok(rb_scheduler_next_action(&core, 100000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC_DISCOVERY);
	zassert_equal(rb_scheduler_next_action(&core, 100000, &action), -EAGAIN);
	rb_scheduler_on_radio_event(&core, &tx_success, 100000);
	zassert_ok(rb_scheduler_next_action(&core, 100000, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);

	rb_scheduler_set_free_slots(&core, 0);
	rb_scheduler_set_sync_deadline(&core, 200000);
	zassert_ok(rb_scheduler_next_action(&core, 200000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC_DISCOVERY);
	zassert_equal(rb_scheduler_next_action(&core, 200000, &action), -EAGAIN);
	rb_scheduler_on_radio_event(&core, &tx_success, 200000);
	zassert_ok(rb_scheduler_next_action(&core, 200000, &action));
	zassert_not_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);
}

ZTEST(master_scheduler, test_uart_aggregation_queue_drops_oldest_bytes)
{
	static uint8_t big[RB_SCHEDULER_UART_QUEUE_SIZE + 100];

	init_master();
	for (size_t i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)i;
	}
	/* A single write larger than the queue keeps only the newest capacity
	 * bytes, matching rb_byte_ring's DROP_OLDEST policy for the UART rings
	 * this internal aggregation queue feeds into. */
	zassert_equal(rb_scheduler_uart_write(&core, big, sizeof(big), 0),
		      sizeof(big));
	zassert_equal(rb_scheduler_queue_drop_bytes(&core), 100);
	zassert_equal(core.uart_count, RB_SCHEDULER_UART_QUEUE_SIZE);
	zassert_equal(core.uart_queue[0], (uint8_t)100);

	/* A second write that individually fits, but overflows the already-full
	 * queue, must also evict the oldest bytes rather than reject the new
	 * write or overwrite past the buffer. */
	{
		const uint8_t more[3] = {0xaa, 0xbb, 0xcc};

		zassert_equal(rb_scheduler_uart_write(&core, more, sizeof(more), 0),
			      sizeof(more));
	}
	zassert_equal(rb_scheduler_queue_drop_bytes(&core), 103);
	zassert_equal(core.uart_count, RB_SCHEDULER_UART_QUEUE_SIZE);
}

ZTEST(master_scheduler, test_uart_available_reports_non_evicting_capacity)
{
	const uint8_t payload[] = {1u, 2u, 3u};

	init_master();
	zassert_equal(rb_scheduler_uart_available(&core),
		      RB_SCHEDULER_UART_QUEUE_SIZE);
	zassert_equal(rb_scheduler_uart_write(&core, payload, sizeof(payload), 1u),
		      sizeof(payload));
	zassert_equal(rb_scheduler_uart_available(&core),
		      RB_SCHEDULER_UART_QUEUE_SIZE - sizeof(payload));
	zassert_equal(rb_scheduler_uart_available(NULL), 0u);
}

ZTEST(master_scheduler, test_assign_waits_for_full_response_window)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 0,
	};
	struct rb_radio_event_view tx_success = {
		.type = RB_EVENT_VIEW_TX_SUCCESS,
	};
	struct rb_hello hello = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 7, 0, 0),
		.device_id = 30,
		.discovery_nonce = 0x44,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	rb_scheduler_set_free_slots(&core, 3);
	/* Sending SYNC_DISCOVERY opens an 8 * 500us = 4000us response window; a
	 * candidate does not switch to its temporary ASSIGN_PRX address until
	 * that window elapses, so ASSIGN must not be produced before then even
	 * if a HELLO already arrived. */
	zassert_ok(rb_scheduler_next_action(&core, 100000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC_DISCOVERY);
	rb_scheduler_on_radio_event(&core, &tx_success, 100000);
	zassert_ok(rb_scheduler_next_action(&core, 100000, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);

	zassert_ok(rb_hello_encode(&hello, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 100100);

	zassert_equal(rb_scheduler_next_action(&core, 103999, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 104000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(action.target_device_id, 30);
}

ZTEST(master_scheduler, test_hello_arrival_order_and_ack_activation)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 0,
	};
	struct rb_hello hello = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 7, 0, 0),
		.discovery_nonce = 0x44,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	rb_scheduler_set_free_slots(&core, 3);
	for (uint64_t device_id = 30; device_id != 0; device_id -= 10) {
		hello.device_id = device_id;
		zassert_ok(rb_hello_encode(&hello, wire, sizeof(wire), &wire_len));
		event.wire = wire;
		event.wire_len = wire_len;
		rb_scheduler_on_radio_event(&core, &event, 10);
	}
	/* Each ASSIGN switches to a temporary ESB profile for a different
	 * candidate; the next ASSIGN must wait for the previous one's hardware
	 * TX_SUCCESS/TX_FAILED before the runtime is allowed to switch profile
	 * again, so the test drives that event between each ASSIGN. */
	zassert_ok(rb_scheduler_next_action(&core, 1000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(action.node_id, 1);
	zassert_equal(rb_scheduler_active_count(&core), 0);
	zassert_equal(rb_scheduler_next_action(&core, 1000, &action), -EAGAIN);

	event.type = RB_EVENT_VIEW_TX_SUCCESS;
	event.pipe = 1;
	event.wire = NULL;
	event.wire_len = 0;
	rb_scheduler_on_radio_event(&core, &event, 1001);
	zassert_equal(rb_scheduler_active_count(&core), 1);

	zassert_ok(rb_scheduler_next_action(&core, 1000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(action.node_id, 2);
	event.pipe = 2;
	rb_scheduler_on_radio_event(&core, &event, 1002);
	zassert_equal(rb_scheduler_active_count(&core), 2);

	zassert_ok(rb_scheduler_next_action(&core, 1000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(action.node_id, 3);
	event.pipe = 3;
	rb_scheduler_on_radio_event(&core, &event, 1003);
	zassert_equal(rb_scheduler_active_count(&core), 3);
}

ZTEST(master_scheduler, test_expired_peer_can_rejoin_with_same_device_id)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 0,
	};
	struct rb_radio_event_view tx_success = {
		.type = RB_EVENT_VIEW_TX_SUCCESS,
	};
	struct rb_hello hello = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 7, 0, 0),
		.device_id = 30,
		.discovery_nonce = 0x44,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	zassert_ok(rb_hello_encode(&hello, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 10);
	zassert_ok(rb_scheduler_next_action(&core, 10, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	rb_scheduler_on_radio_event(&core, &tx_success, 11);
	zassert_equal(rb_scheduler_active_count(&core), 1);
	core.peer[0].uplink_ack_base = 77u;

	/* Expire the active lease, open the next discovery window, and present the
	 * same physical slave again.  Historical candidate bookkeeping must not
	 * suppress the new HELLO after membership has released the device. */
	zassert_ok(rb_scheduler_next_action(&core, 100011, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC_DISCOVERY);
	rb_scheduler_on_radio_event(&core, &tx_success, 100011);
	zassert_ok(rb_scheduler_next_action(&core, 100011, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_DISCOVERY_RX);
	rb_scheduler_on_radio_event(&core, &event, 100100);
	zassert_ok(rb_scheduler_next_action(&core, 104011, &action));
	zassert_equal(action.type, RB_ACTION_SEND_ASSIGN);
	zassert_equal(action.target_device_id, hello.device_id);
	zassert_equal(core.peer[0].uplink_ack_base, 0u);
}

ZTEST(master_scheduler, test_aggregation_timeout_idle_backoff_and_reset)
{
	struct rb_scheduler_action action;
	const uint8_t byte = 0x55;

	init_master();
	zassert_equal(rb_scheduler_uart_write(&core, &byte, 1, 100), 1);
	zassert_equal(rb_scheduler_next_action(&core, 1099, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 1100, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);

	rb_scheduler_init(&core, &(struct rb_scheduler_config){
		.master = true,
		.master_session = 7,
		.sync_interval_us = 100000,
		.max_idle_poll_us = 5000,
		.lease_timeout_us = 100000,
	});
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	for (int i = 0; i < 8; i++) {
		rb_scheduler_mark_peer_idle(&core, 1, (uint64_t)i * 10000);
	}
	zassert_true(rb_scheduler_peer_poll_interval(&core, 1) <= 5000);
	rb_scheduler_mark_peer_data(&core, 1, 100000);
	zassert_equal(rb_scheduler_peer_poll_interval(&core, 1), 0);
	zassert_ok(rb_scheduler_reset_session(&core, 9));
	zassert_equal(rb_scheduler_session(&core), 9);
	zassert_equal(rb_scheduler_next_action(&core, 0, &action), -EAGAIN);
	zassert_equal(rb_scheduler_reset_session(&core, 0), -EINVAL);
}

ZTEST(master_scheduler, test_empty_ack_applies_idle_poll_backoff)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_TX_SUCCESS,
		.pipe = 1,
	};
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7, 1, 1),
		.uplink_epoch = 1,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_next_action(&core, 0, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	rb_scheduler_on_radio_event(&core, &event, 1);
	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &wire_len));
	event.type = RB_EVENT_VIEW_RX_RECEIVED;
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 1);
	zassert_equal(rb_scheduler_peer_poll_interval(&core, 1), 2000);
	zassert_equal(rb_scheduler_duplicate_count(&core), 0u);
	zassert_equal(rb_scheduler_next_action(&core, 2000, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 2001, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
}

ZTEST(master_scheduler, test_uplink_payload_is_delivered_once_and_acknowledged)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 1,
	};
	const uint8_t payload[] = {0xa5, 0x5a};
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7, 1, 1),
		.uplink_epoch = 1,
		.uplink_sequence = 1,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	struct rb_poll poll;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	uint8_t received[sizeof(payload)];
	size_t wire_len;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 1);
	zassert_equal(rb_scheduler_master_read_uart(&core, received,
						 sizeof(received)), sizeof(payload));
	zassert_mem_equal(received, payload, sizeof(payload));
	zassert_equal(rb_scheduler_duplicate_count(&core), 0u);

	zassert_ok(rb_scheduler_next_action(&core, 1, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_ok(rb_poll_decode(action.wire, action.wire_len, &poll));
	zassert_equal(poll.uplink_ack_base, ack.uplink_sequence);

	rb_scheduler_on_radio_event(&core, &event, 2);
	zassert_equal(rb_scheduler_master_read_uart(&core, received,
						 sizeof(received)), 0u);
	zassert_equal(rb_scheduler_duplicate_count(&core), 1u);
}

ZTEST(master_scheduler, test_broadcast_loss_does_not_schedule_repair)
{
	struct rb_scheduler_action action;
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = 1,
	};
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7, 1, 1),
		.uplink_epoch = 1,
	};
	const uint8_t payload = 0xa5;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	init_master();
	zassert_ok(rb_scheduler_add_active_peer(&core, 1, 0x11, 1, 0));
	zassert_ok(rb_scheduler_queue_downlink(&core, &payload, 1));
	zassert_ok(rb_scheduler_next_action(&core, 0, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);
	complete_master_tx(0);
	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&core, &event, 1);
	zassert_equal(rb_scheduler_next_action(&core, 2000, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 2001, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
}

ZTEST(master_scheduler, test_slave_hello_preserves_sync_session_and_nonce)
{
	struct rb_scheduler_core slave;
	struct rb_scheduler_config config = {
		.master = false,
		.group_id = 41,
		.master_session = 7,
		.device_id = 10,
		.sync_interval_us = 100000,
		.lease_timeout_us = 100000,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_sync_discovery sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0),
		.group_id = 41,
		.sync_sequence = 2,
		.discovery_nonce = 0xdeadbeef,
		.free_slots = 1,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.address_tick = 10000,
	};
	struct rb_scheduler_action action;
	struct rb_hello hello;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	rb_scheduler_init(&slave, &config);
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&slave, &event, event.address_tick);
	zassert_ok(rb_scheduler_next_action(&slave, 11000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_HELLO);
	zassert_ok(rb_hello_decode(action.wire, action.wire_len, &hello));
	zassert_equal(hello.common.master_session, 9);
	zassert_equal(hello.discovery_nonce, sync.discovery_nonce);
}

ZTEST(master_scheduler, test_slave_assignment_window_returns_to_group_rx)
{
	struct rb_scheduler_core slave;
	struct rb_scheduler_config config = {
		.master = false,
		.group_id = 41,
		.master_session = 7,
		.device_id = 10,
		.sync_interval_us = 100000,
		.lease_timeout_us = 100000,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_sync_discovery sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 9, 0, 0),
		.group_id = 41,
		.sync_sequence = 2,
		.discovery_nonce = 0xdeadbeef,
		.free_slots = 1,
		.response_slot_count = 8,
		.response_slot_us = 500,
	};
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.address_tick = 10000,
	};
	struct rb_scheduler_action action;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;

	rb_scheduler_init(&slave, &config);
	zassert_ok(rb_sync_discovery_encode(&sync, wire, sizeof(wire), &wire_len));
	event.wire = wire;
	event.wire_len = wire_len;
	rb_scheduler_on_radio_event(&slave, &event, event.address_tick);
	zassert_ok(rb_scheduler_next_action(&slave, 11000, &action));
	zassert_equal(action.type, RB_ACTION_SEND_HELLO);
	event.type = RB_EVENT_VIEW_TX_SUCCESS;
	event.wire = NULL;
	event.wire_len = 0;
	rb_scheduler_on_radio_event(&slave, &event, 11050);
	zassert_ok(rb_scheduler_next_action(&slave, 15000, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_ASSIGN_RX);
	zassert_ok(rb_scheduler_next_action(&slave, 21000, &action));
	zassert_equal(action.type, RB_ACTION_ENTER_GROUP_RX);
}

ZTEST_SUITE(master_scheduler, NULL, NULL, NULL, NULL, NULL);
