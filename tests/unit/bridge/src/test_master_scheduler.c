#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "link_scheduler_core.h"

static struct rb_scheduler_core core;

static void init_master(void)
{
	const struct rb_scheduler_config config = {
		.master = true,
		.node_id = 0u,
		.group_id = 1u,
		.master_session = 7u,
		.local_device_id = 0x100u,
		.sync_interval_us = 100000u,
		.aggregation_timeout_us = 1000u,
		.max_idle_poll_us = 5000u,
		.lease_timeout_us = 100000u,
	};

	rb_scheduler_init(&core, &config);
}

static void complete_tx(enum rb_radio_event_view_type type, uint64_t now_us)
{
	const struct rb_radio_event_view event = {.type = type};

	rb_scheduler_on_radio_event(&core, &event, now_us);
}

static void deliver_ack_for_session(uint8_t pipe, uint8_t source_node,
				    uint32_t master_session,
				    uint32_t slave_session,
				    uint32_t sequence,
				    const uint8_t *payload, size_t payload_len,
				    uint64_t now_us)
{
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, master_session,
					 source_node),
		.slave_session = slave_session,
		.uplink_sequence = sequence,
		.payload = payload,
		.payload_len = payload_len,
	};
	struct rb_radio_event_view event = {
		.type = RB_EVENT_VIEW_RX_RECEIVED,
		.pipe = pipe,
	};
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len));
	event.wire = wire;
	event.wire_len = len;
	rb_scheduler_on_radio_event(&core, &event, now_us);
}

static void deliver_ack(uint8_t node_id, uint32_t slave_session,
			uint32_t sequence, const uint8_t *payload,
			size_t payload_len, uint64_t now_us)
{
	deliver_ack_for_session(node_id, node_id, 7u, slave_session, sequence,
				payload, payload_len, now_us);
}

static void activate_all(void)
{
	deliver_ack(1u, 0x11u, 0u, NULL, 0u, 1u);
	deliver_ack(2u, 0x22u, 0u, NULL, 0u, 2u);
	deliver_ack(3u, 0x33u, 0u, NULL, 0u, 3u);
	core.next_probe_due_us = UINT64_MAX;
}

ZTEST(master_scheduler, test_inactive_nodes_are_polled_round_robin)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_ok(rb_scheduler_next_action(&core, 0u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	zassert_equal(action.node_id, 1u);
	complete_tx(RB_EVENT_VIEW_TX_FAILED, 1u);
	zassert_equal(rb_scheduler_next_action(&core, 19999u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 20000u, &action));
	zassert_equal(action.node_id, 2u);
}

ZTEST(master_scheduler, test_only_one_probe_may_precede_overdue_sync)
{
	struct rb_scheduler_action action;

	init_master();
	rb_scheduler_set_sync_deadline(&core, 1000u);
	zassert_ok(rb_scheduler_next_action(&core, 1000u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_POLL);
	complete_tx(RB_EVENT_VIEW_TX_FAILED, 1001u);
	zassert_ok(rb_scheduler_next_action(&core, 1001u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC);
}

ZTEST(master_scheduler, test_valid_ack_activates_fixed_node)
{
	init_master();
	deliver_ack(2u, 0xabcdef01u, 0u, NULL, 0u, 1000u);
	zassert_equal(rb_scheduler_active_mask(&core), BIT(1));
	zassert_equal(rb_scheduler_active_count(&core), 1u);
}

ZTEST(master_scheduler, test_ack_pipe_source_and_session_are_validated)
{
	init_master();
	deliver_ack_for_session(1u, 2u, 7u, 0x11u, 0u, NULL, 0u, 1000u);
	deliver_ack_for_session(1u, 1u, 6u, 0x11u, 0u, NULL, 0u, 2000u);
	zassert_equal(rb_scheduler_active_count(&core), 0u);
	zassert_equal(rb_scheduler_invalid_node_count(&core), 1u);
	zassert_equal(rb_scheduler_invalid_session_count(&core), 1u);
}

ZTEST(master_scheduler, test_slave_restart_resets_only_its_uplink_state)
{
	const uint8_t first[] = {1u};
	const uint8_t second[] = {2u};

	init_master();
	deliver_ack(1u, 0x11u, 8u, first, sizeof(first), 1000u);
	deliver_ack(2u, 0x22u, 4u, second, sizeof(second), 1001u);
	deliver_ack(1u, 0x33u, 1u, first, sizeof(first), 2000u);
	zassert_equal(core.fixed_peers.peers[0].slave_session, 0x33u);
	zassert_equal(core.peer[0].uplink_ack_sequence, 1u);
	zassert_equal(core.fixed_peers.peers[1].slave_session, 0x22u);
	zassert_equal(core.peer[1].uplink_ack_sequence, 4u);
}

ZTEST(master_scheduler, test_expired_node_returns_to_probe_and_rejoins)
{
	struct rb_scheduler_action action;

	init_master();
	deliver_ack(1u, 0x11u, 0u, NULL, 0u, 1000u);
	core.next_probe_due_us = UINT64_MAX;
	zassert_equal(rb_scheduler_active_count(&core), 1u);
	zassert_ok(rb_scheduler_next_action(&core, 101000u, &action));
	zassert_equal(rb_scheduler_active_count(&core), 0u);
	deliver_ack(1u, 0x11u, 0u, NULL, 0u, 102000u);
	zassert_equal(rb_scheduler_active_count(&core), 1u);
}

ZTEST(master_scheduler, test_sync_publication_is_encoded)
{
	struct rb_scheduler_action action;
	struct rb_sync_frame sync;

	init_master();
	activate_all();
	zassert_ok(rb_scheduler_set_time_publication(&core, 1700000000,
					     RB_TIME_LOCKED));
	rb_scheduler_set_sync_deadline(&core, 100000u);
	zassert_ok(rb_scheduler_next_action(&core, 100000u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_SYNC);
	zassert_ok(rb_sync_decode(action.wire, action.wire_len, &sync));
	zassert_equal(sync.next_pps_utc_seconds, 1700000000);
	zassert_equal(sync.time_quality, RB_TIME_LOCKED);
}

ZTEST(master_scheduler, test_broadcast_is_best_effort_and_waits_for_completion)
{
	const uint8_t payload[] = {0x55u};
	struct rb_scheduler_action action;
	struct rb_downlink_frame downlink;

	init_master();
	activate_all();
	zassert_equal(rb_scheduler_uart_write(&core, payload, sizeof(payload), 1u),
		      sizeof(payload));
	zassert_equal(rb_scheduler_next_action(&core, 1000u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 1001u, &action));
	zassert_equal(action.type, RB_ACTION_SEND_DOWNLINK_BROADCAST);
	zassert_true(action.no_ack);
	zassert_ok(rb_downlink_decode(action.wire, action.wire_len, &downlink));
	zassert_mem_equal(downlink.payload, payload, sizeof(payload));
	zassert_equal(rb_scheduler_next_action(&core, 1001u, &action), -EAGAIN);
}

ZTEST(master_scheduler, test_poll_failure_uses_bounded_retry_backoff)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_ok(rb_scheduler_next_action(&core, 0u, &action));
	rb_scheduler_action_failed(&core, &action, 0u);
	zassert_equal(rb_scheduler_next_action(&core, 999u, &action), -EAGAIN);
	zassert_ok(rb_scheduler_next_action(&core, 1000u, &action));
}

ZTEST(master_scheduler, test_ack_age_is_unknown_until_first_ack)
{
	init_master();
	zassert_equal(rb_scheduler_peer_ack_age_us(&core, 1u, 500u), -1);
	deliver_ack(1u, 0x11u, 0u, NULL, 0u, 1000u);
	zassert_equal(rb_scheduler_peer_ack_age_us(&core, 1u, 1500u), 500);
	zassert_equal(rb_scheduler_peer_ack_age_us(&core, 4u, 1500u), -1);
}

ZTEST(master_scheduler, test_poll_failures_are_counted_per_node)
{
	struct rb_scheduler_action action;

	init_master();
	zassert_ok(rb_scheduler_next_action(&core, 0u, &action));
	zassert_equal(action.node_id, 1u);
	complete_tx(RB_EVENT_VIEW_TX_FAILED, 1u);
	zassert_equal(rb_scheduler_peer_poll_failure_count(&core, 1u), 1u);
	zassert_equal(rb_scheduler_peer_poll_failure_count(&core, 2u), 0u);

	zassert_ok(rb_scheduler_next_action(&core, 20000u, &action));
	zassert_equal(action.node_id, 2u);
	complete_tx(RB_EVENT_VIEW_TX_FAILED, 20001u);
	zassert_equal(rb_scheduler_peer_poll_failure_count(&core, 1u), 1u);
	zassert_equal(rb_scheduler_peer_poll_failure_count(&core, 2u), 1u);
}

ZTEST(master_scheduler, test_idle_poll_interval_caps_and_data_resets_it)
{
	init_master();
	activate_all();
	for (uint64_t now = 10u; now < 100u; now += 10u) {
		rb_scheduler_mark_peer_idle(&core, 1u, now);
	}
	zassert_equal(rb_scheduler_peer_poll_interval(&core, 1u), 5000u);
	rb_scheduler_mark_peer_data(&core, 1u, 100u);
	zassert_equal(rb_scheduler_peer_poll_interval(&core, 1u), 0u);
}

ZTEST(master_scheduler, test_uart_queue_keeps_newest_bytes_and_counts_drop)
{
	uint8_t full[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint8_t more[3] = {1u, 2u, 3u};

	init_master();
	zassert_equal(rb_scheduler_uart_write(&core, full, sizeof(full), 1u),
		      sizeof(full));
	zassert_equal(rb_scheduler_uart_write(&core, more, sizeof(more), 2u),
		      sizeof(more));
	zassert_equal(rb_scheduler_queue_drop_bytes(&core), sizeof(more));
}

ZTEST_SUITE(master_scheduler, NULL, NULL, NULL, NULL, NULL);
