#include "link_scheduler_core.h"

#include <errno.h>
#include <string.h>

#define RB_SCHEDULER_DEFAULT_RESPONSE_SLOTS 8u
#define RB_SCHEDULER_DEFAULT_RESPONSE_SLOT_US 500u
#define RB_SCHEDULER_DEFAULT_AGGREGATION_US 1000u
#define RB_SCHEDULER_DEFAULT_SYNC_US 100000u
#define RB_SCHEDULER_DEFAULT_LEASE_US 100000u

static bool node_valid(uint8_t node_id)
{
	return node_id >= 1u && node_id <= RB_SCHEDULER_MAX_PEERS;
}

static uint16_t next_nonzero_u16(uint16_t *value)
{
	uint16_t result = *value;

	(*value)++;
	if (*value == 0u) {
		*value = 1u;
	}
	if (result == 0u) {
		result = *value;
		(*value)++;
		if (*value == 0u) {
			*value = 1u;
		}
	}
	return result;
}

static struct rb_scheduler_peer_runtime *peer_runtime(
	struct rb_scheduler_core *core, uint8_t node_id)
{
	if (core == NULL || !node_valid(node_id)) {
		return NULL;
	}
	return &core->peer[node_id - 1u];
}

static const struct rb_scheduler_peer_runtime *peer_runtime_const(
	const struct rb_scheduler_core *core, uint8_t node_id)
{
	if (core == NULL || !node_valid(node_id)) {
		return NULL;
	}
	return &core->peer[node_id - 1u];
}

static bool peer_schedulable(const struct rb_scheduler_core *core,
				     uint8_t node_id)
{
	const struct rb_membership_peer *peer =
		rb_membership_peer(&core->membership, node_id);

	return peer != NULL &&
		(peer->state == RB_PEER_ACTIVE || peer->state == RB_PEER_SUSPECT);
}

static bool membership_has_device(const struct rb_scheduler_core *core,
				  uint64_t device_id)
{
	for (uint8_t node_id = 1u; node_id <= RB_SCHEDULER_MAX_PEERS; node_id++) {
		const struct rb_membership_peer *peer =
			rb_membership_peer(&core->membership, node_id);

		if (peer->state != RB_PEER_FREE && peer->device_id == device_id) {
			return true;
		}
	}
	return false;
}

static uint32_t idle_limit(const struct rb_scheduler_core *core)
{
	if (core->config.max_idle_poll_us != 0u) {
		return core->config.max_idle_poll_us;
	}
	return 5000u;
}

static void clear_uart(struct rb_scheduler_core *core)
{
	core->uart_head = 0u;
	core->uart_count = 0u;
	core->first_uart_tick = 0u;
}

static size_t uart_peek(const struct rb_scheduler_core *core,
			uint8_t *out, size_t max_len)
{
	size_t n = core->uart_count < max_len ? core->uart_count : max_len;

	for (size_t i = 0; i < n; i++) {
		out[i] = core->uart_queue[(core->uart_head + i) %
					RB_SCHEDULER_UART_QUEUE_SIZE];
	}
	return n;
}

static void uart_discard(struct rb_scheduler_core *core, size_t len)
{
	if (len > core->uart_count) {
		len = core->uart_count;
	}
	core->uart_head = (uint16_t)((core->uart_head + len) %
				     RB_SCHEDULER_UART_QUEUE_SIZE);
	core->uart_count = (uint16_t)(core->uart_count - len);
	if (core->uart_count == 0u) {
		core->first_uart_tick = 0u;
	}
}

static size_t queue_peek(const uint8_t *queue, uint16_t head, uint16_t count,
				uint8_t *out, size_t max_len)
{
	size_t n = count < max_len ? count : max_len;
	for (size_t i = 0; i < n; i++) {
		out[i] = queue[(head + i) % RB_SCHEDULER_UART_QUEUE_SIZE];
	}
	return n;
}

/* DROP_OLDEST append: when the fixed-size queue cannot hold the incoming
 * bytes, oldest queued bytes (and, if the write itself exceeds capacity, the
 * oldest part of the write) are discarded first so the newest data is always
 * kept, mirroring rb_byte_ring_write()'s policy for these scheduler-internal
 * aggregation/output queues. Returns the number of incoming bytes retained
 * (always len, unless the queue has zero capacity), and accumulates any
 * evicted byte count into *dropped_bytes. */
static size_t queue_append(uint8_t *queue, uint16_t *head, uint16_t *count,
				   const uint8_t *data, size_t len,
				   uint64_t *dropped_bytes)
{
	size_t overflow;

	if (queue == NULL || head == NULL || count == NULL || data == NULL ||
		len == 0u) {
		return 0;
	}
	if (len >= RB_SCHEDULER_UART_QUEUE_SIZE) {
		size_t original_len = len;
		size_t skipped = len - RB_SCHEDULER_UART_QUEUE_SIZE;

		if (dropped_bytes != NULL) {
			*dropped_bytes += *count + skipped;
		}
		data += skipped;
		len = RB_SCHEDULER_UART_QUEUE_SIZE;
		*head = 0u;
		*count = 0u;
		memcpy(queue, data, len);
		*count = (uint16_t)len;
		return original_len;
	}
	overflow = (size_t)*count + len > RB_SCHEDULER_UART_QUEUE_SIZE ?
		(size_t)*count + len - RB_SCHEDULER_UART_QUEUE_SIZE : 0u;
	if (overflow != 0u) {
		*head = (uint16_t)((*head + overflow) % RB_SCHEDULER_UART_QUEUE_SIZE);
		*count = (uint16_t)(*count - overflow);
		if (dropped_bytes != NULL) {
			*dropped_bytes += overflow;
		}
	}
	for (size_t i = 0; i < len; i++) {
		uint16_t index = (uint16_t)((*head + *count) %
						RB_SCHEDULER_UART_QUEUE_SIZE);
		queue[index] = data[i];
		(*count)++;
	}
	return len;
}

static void queue_discard(uint16_t *head, uint16_t *count, size_t len)
{
	if (head == NULL || count == NULL) {
		return;
	}
	if (len > *count) {
		len = *count;
	}
	*head = (uint16_t)((*head + len) % RB_SCHEDULER_UART_QUEUE_SIZE);
	*count = (uint16_t)(*count - len);
}

static void slave_prepare_ack(struct rb_scheduler_core *core,
				      uint16_t poll_sequence)
{
	uint8_t payload[RB_ACK_UPLINK_PAYLOAD_MAX];
	size_t payload_len = 0;
	uint32_t payload_sequence = 0u;
	struct rb_ack_uplink ack;
	(void)poll_sequence;

	if (core->slave_uplink_inflight) {
		payload_len = queue_peek(core->slave_uplink_queue,
					 core->slave_uplink_head,
					 core->slave_uplink_inflight_len, payload,
					 sizeof(payload));
		payload_sequence = core->slave_uplink_inflight_sequence;
	} else if (core->slave_uplink_count != 0u) {
		payload_len = queue_peek(core->slave_uplink_queue,
					 core->slave_uplink_head,
					 core->slave_uplink_count, payload,
					 sizeof(payload));
		if (payload_len != 0u) {
			payload_sequence = core->slave_uplink_sequence++;
			if (core->slave_uplink_sequence == 0u) {
				core->slave_uplink_sequence = 1u;
			}
			core->slave_uplink_inflight = true;
			core->slave_uplink_inflight_sequence = payload_sequence;
			core->slave_uplink_inflight_len = payload_len;
		}
	}
	ack = (struct rb_ack_uplink){
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK,
					 core->config.master_session,
					 core->slave_lease_id, core->slave_node_id),
		.uplink_epoch = (uint16_t)core->slave_uplink_epoch,
		.uplink_sequence = payload_sequence,
		.downlink_epoch = (uint16_t)core->slave_downlink_epoch,
		.downlink_ack_base = core->slave_rx_window.ack_base,
		.downlink_ack_bitmap = core->slave_rx_window.ack_bitmap,
		.drop_count = 0,
		.payload = payload_len ? payload : NULL,
		.payload_len = payload_len,
	};
	if (rb_ack_uplink_encode(&ack, core->slave_ack_wire,
				 sizeof(core->slave_ack_wire),
				 &core->slave_ack_wire_len) == 0) {
		core->slave_ack_ready = true;
		core->slave_last_poll_sequence = poll_sequence;
	}
}

static void action_clear(struct rb_scheduler_action *action)
{
	memset(action, 0, sizeof(*action));
	action->type = RB_ACTION_NONE;
}

static bool action_uses_transaction_gate(enum rb_scheduler_action_type type)
{
	switch (type) {
	case RB_ACTION_SEND_SYNC_DISCOVERY:
	case RB_ACTION_SEND_ASSIGN:
	case RB_ACTION_SEND_DOWNLINK_BROADCAST:
	case RB_ACTION_SEND_REPAIR:
	case RB_ACTION_SEND_SKIP_TO:
	case RB_ACTION_SEND_POLL:
	case RB_ACTION_SEND_HELLO:
		return true;
	default:
		return false;
	}
}

static int build_poll(struct rb_scheduler_core *core, uint8_t node_id,
			      uint64_t now_us, struct rb_scheduler_action *action)
{
	const struct rb_membership_peer *peer =
		rb_membership_peer(&core->membership, node_id);
	struct rb_poll frame;
	struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);

	if (peer == NULL || runtime == NULL || !peer_schedulable(core, node_id)) {
		return -ENOENT;
	}
	frame = (struct rb_poll){
		.common = RB_COMMON_INIT(RB_FRAME_POLL, core->config.master_session,
					 peer->lease_id, 0),
		.uplink_epoch = (uint16_t)core->uplink_epoch,
		.uplink_ack_base = runtime->uplink_ack_base,
		.uplink_ack_bitmap = 0,
		.next_credit_bytes = RB_ACK_UPLINK_PAYLOAD_MAX,
		.poll_sequence = (uint16_t)runtime->poll_sequence++,
	};
	if (rb_poll_encode(&frame, action->wire, sizeof(action->wire),
			   &action->wire_len) != 0) {
		return -EINVAL;
	}
	action->type = RB_ACTION_SEND_POLL;
	action->node_id = node_id;
	action->pipe = node_id;
	action->no_ack = false;
	action->due_tick = now_us;
	runtime->next_poll_due_us = (uint32_t)now_us + runtime->poll_interval_us;
	core->in_flight_poll_data_ready = runtime->data_ready;
	runtime->poll_retry_backoff = false;
	runtime->data_ready = false;
	core->transaction_in_flight = true;
	core->in_flight_node = node_id;
	core->in_flight_pipe = node_id;
	core->in_flight_type = action->type;
	return 0;
}

static int build_sync(struct rb_scheduler_core *core, uint64_t now_us,
			      struct rb_scheduler_action *action)
{
	struct rb_sync_discovery frame = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY,
					 core->config.master_session, 0, 0),
		.group_id = core->config.group_id,
		.master_id = core->config.device_id,
		.sync_sequence = (uint32_t)(now_us / (core->config.sync_interval_us ?
						core->config.sync_interval_us :
						RB_SCHEDULER_DEFAULT_SYNC_US)) + 1u,
		.sync_interval_us = core->config.sync_interval_us,
		.discovery_nonce = (uint32_t)now_us ^ core->config.master_session,
		.free_slots = core->free_slots,
		.response_slot_count = core->config.response_slot_count,
		.response_slot_us = core->config.response_slot_us,
	};

	if (rb_sync_discovery_encode(&frame, action->wire, sizeof(action->wire),
				     &action->wire_len) != 0) {
		return -EINVAL;
	}
	action->type = RB_ACTION_SEND_SYNC_DISCOVERY;
	action->pipe = 0;
	action->no_ack = true;
	action->due_tick = now_us;
	core->next_sync_tick = now_us + (core->config.sync_interval_us ?
					 core->config.sync_interval_us :
					 RB_SCHEDULER_DEFAULT_SYNC_US);
	core->discovery_rx_pending = core->free_slots != 0u;
	/* The candidate only switches to its temporary ASSIGN_PRX address after
	 * the full response window elapses (see slave_handle_rx()'s
	 * slave_assign_rx_deadline).  ASSIGN must not be sent before that, or it
	 * targets an address the candidate has not started listening on yet. */
	core->response_window_deadline = core->discovery_rx_pending ?
		now_us + (uint64_t)frame.response_slot_count *
			(frame.response_slot_us ? frame.response_slot_us : 500u) : 0u;
	core->transaction_in_flight = true;
	core->in_flight_node = 0u;
	core->in_flight_pipe = 0u;
	core->in_flight_type = action->type;
	return 0;
}

static int build_broadcast(struct rb_scheduler_core *core, uint64_t now_us,
				   struct rb_scheduler_action *action)
{
	uint8_t payload[RB_PACKET_DATA_MAX];
	size_t payload_len;
	uint32_t sequence = core->next_downlink_sequence;
	const uint8_t *history_payload = payload;
	size_t history_payload_len = 0u;
	bool sequence_already_stored;

	payload_len = uart_peek(core, payload, sizeof(payload));
	if (payload_len == 0u && !core->queued_broadcast) {
		return -EAGAIN;
	}
	if (core->queued_broadcast) {
		memcpy(action->wire, core->queued_wire, core->queued_wire_len);
		action->wire_len = core->queued_wire_len;
		{
			struct rb_data_frame queued;
			if (rb_data_decode(action->wire, action->wire_len, &queued) != 0) {
				return -EBADMSG;
			}
			sequence = queued.sequence;
			history_payload = queued.payload;
			history_payload_len = queued.payload_len;
		}
		core->queued_broadcast = false;
	} else {
		struct rb_data_frame frame = {
			.common = RB_COMMON_INIT(RB_FRAME_DOWNLINK_DATA,
						 core->config.master_session, 0, 0),
			.stream_epoch = (uint16_t)core->downlink_epoch,
			.sequence = sequence,
			.payload = payload,
			.payload_len = payload_len,
		};
		if (rb_data_encode(frame.common.type, frame.common.master_session,
				   frame.common.lease_id, frame.stream_epoch,
				   frame.sequence, frame.payload, frame.payload_len,
				   action->wire, sizeof(action->wire),
				   &action->wire_len) != 0) {
			return -EINVAL;
		}
		uart_discard(core, payload_len);
		history_payload_len = payload_len;
	}
	sequence_already_stored = rb_tx_window_contains(&core->downlink_history,
						       sequence);
	if (!sequence_already_stored &&
	    rb_tx_window_store(&core->downlink_history, sequence, history_payload,
				 history_payload_len) != 0) {
		return -EINVAL;
	}
	if (sequence == core->next_downlink_sequence) {
		core->next_downlink_sequence++;
		if (core->next_downlink_sequence == 0u) {
			core->next_downlink_sequence = 1u;
		}
	}
	action->type = RB_ACTION_SEND_DOWNLINK_BROADCAST;
	action->pipe = 0;
	action->no_ack = true;
	action->due_tick = now_us;
	core->transaction_in_flight = true;
	core->in_flight_node = 0u;
	core->in_flight_pipe = 0u;
	core->in_flight_type = action->type;
	return 0;
}

static int build_repair_or_skip(struct rb_scheduler_core *core, uint8_t node_id,
					uint64_t now_us,
					struct rb_scheduler_action *action)
{
	struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);
	const struct rb_packet_slot *slot;

	if (runtime == NULL || !peer_schedulable(core, node_id)) {
		return -ENOENT;
	}
	if (runtime->skip_pending) {
		struct rb_skip_to frame = {
			.common = RB_COMMON_INIT(RB_FRAME_SKIP_TO,
						 core->config.master_session,
						 rb_membership_peer(&core->membership, node_id)->lease_id,
						 0),
			.direction = 0,
			.stream_epoch = (uint16_t)core->downlink_epoch,
			.next_sequence = runtime->missing_sequence,
			.drop_count = 1,
		};
		if (rb_skip_to_encode(&frame, action->wire, sizeof(action->wire),
					      &action->wire_len) != 0) {
			return -EINVAL;
		}
		runtime->skip_pending = false;
		action->type = RB_ACTION_SEND_SKIP_TO;
	} else {
		slot = rb_tx_window_next_repair(&core->downlink_history);
		if (slot == NULL || slot->sequence != runtime->missing_sequence) {
			return -ENOENT;
		}
		if (rb_data_encode(RB_FRAME_REPAIR_DATA,
				   core->config.master_session,
				   rb_membership_peer(&core->membership, node_id)->lease_id,
				   (uint16_t)core->downlink_epoch, slot->sequence,
				   slot->data, slot->data_len, action->wire,
				   sizeof(action->wire), &action->wire_len) != 0) {
			return -EINVAL;
		}
		runtime->repair_pending = false;
		action->type = RB_ACTION_SEND_REPAIR;
	}
	action->node_id = node_id;
	action->pipe = node_id;
	action->no_ack = false;
	action->due_tick = now_us;
	core->transaction_in_flight = true;
	core->in_flight_node = node_id;
	core->in_flight_pipe = node_id;
	core->in_flight_type = action->type;
	return 0;
}

static void slave_handle_rx(struct rb_scheduler_core *core,
				    const struct rb_radio_event_view *event,
				    uint64_t now_us)
{
	struct rb_common_header common;

	if (event->type == RB_EVENT_VIEW_TX_SUCCESS ||
	    event->type == RB_EVENT_VIEW_TX_FAILED) {
		if (core->in_flight_type == RB_ACTION_SEND_HELLO) {
			core->transaction_in_flight = false;
		}
		return;
	}
	if (event->wire == NULL || rb_common_decode(event->wire, event->wire_len,
						   &common) != 0) {
		return;
	}
	if (common.type == RB_FRAME_SYNC_DISCOVERY) {
		struct rb_sync_discovery sync;
		uint8_t count;
		uint8_t slot;
		if (rb_sync_discovery_decode(event->wire, event->wire_len, &sync) != 0 ||
		    sync.group_id != core->config.group_id || sync.free_slots == 0u ||
		    sync.response_slot_count == 0u) {
			return;
		}
		if (core->slave_active &&
		    sync.common.master_session == core->config.master_session) {
			return;
		}
		if (core->slave_active &&
		    sync.common.master_session != core->config.master_session) {
			core->slave_active = false;
			core->slave_node_id = 0u;
			core->slave_lease_id = 0u;
			core->slave_ack_pending = false;
			core->slave_ack_ready = false;
			core->slave_hello_pending = false;
			core->slave_assign_rx_pending = false;
			rb_slave_lease_init(&core->slave_lease,
					    core->config.lease_timeout_us);
		}
		core->config.master_session = sync.common.master_session;
		count = sync.response_slot_count;
		slot = core->slave_discovery_slot_valid ?
			(uint8_t)(core->slave_discovery_slot % count) :
			(uint8_t)(core->config.device_id % count);
		core->slave_hello_due_tick = event->address_tick +
			(uint64_t)slot * (sync.response_slot_us ? sync.response_slot_us : 500u);
		core->slave_assign_rx_deadline = core->slave_hello_due_tick +
			(uint64_t)count * (sync.response_slot_us ? sync.response_slot_us : 500u);
		core->slave_hello_pending = true;
		core->slave_discovery_nonce = sync.discovery_nonce;
		core->slave_assign_rx_pending = true;
		core->slave_assign_rx_active = false;
		return;
	}
	if (common.type == RB_FRAME_ASSIGN) {
		struct rb_assign assign;
		if (rb_assign_decode(event->wire, event->wire_len, &assign) != 0 ||
		    assign.target_device_id != core->config.device_id ||
		    assign.common.master_session != core->config.master_session) {
			return;
		}
		core->slave_active = true;
		core->slave_node_id = assign.node_id;
		core->slave_lease_id = assign.common.lease_id;
		core->slave_downlink_epoch = assign.downlink_epoch;
		core->slave_uplink_epoch = assign.uplink_epoch;
		core->slave_assign_rx_pending = false;
		core->slave_assign_rx_active = false;
		core->slave_group_rx_pending = true;
		core->slave_hello_pending = false;
		(void)rb_slave_lease_assign(&core->slave_lease,
					   core->config.master_session,
					   assign.node_id,
					   assign.common.lease_id,
					   now_us);
		core->slave_ack_pending = false;
		rb_rx_window_reset_epoch(&core->slave_rx_window,
						 (uint16_t)assign.downlink_epoch, 1u);
		slave_prepare_ack(core, 0u);
		return;
	}
	if (common.type == RB_FRAME_POLL && core->slave_active) {
		struct rb_poll poll;
		bool acked = false;
		if (rb_poll_decode(event->wire, event->wire_len, &poll) != 0 ||
		    poll.common.master_session != core->config.master_session ||
		    poll.common.lease_id != core->slave_lease_id ||
		    poll.common.source_node != 0u) {
			return;
		}
		if (rb_slave_lease_note_poll(&core->slave_lease,
					     poll.common.master_session,
					     core->slave_node_id,
					     core->slave_lease_id,
					     now_us) != 0) {
			return;
		}
		if (core->slave_uplink_inflight) {
			uint32_t distance = core->slave_uplink_inflight_sequence -
				poll.uplink_ack_base;
			acked = core->slave_uplink_inflight_sequence == poll.uplink_ack_base ||
				(int32_t)distance < 0 ||
				(distance >= 1u && distance <= 64u &&
				 (poll.uplink_ack_bitmap & (UINT64_C(1) << (distance - 1u))));
			if (acked) {
				queue_discard(&core->slave_uplink_head,
						      &core->slave_uplink_count,
						      core->slave_uplink_inflight_len);
				core->slave_uplink_inflight = false;
				core->slave_uplink_inflight_len = 0u;
			}
		}
		if (core->slave_ack_ready) {
			core->slave_ack_pending = true;
		}
		core->slave_credit_bytes = poll.next_credit_bytes;
		slave_prepare_ack(core, poll.poll_sequence);
		return;
	}
	if ((common.type == RB_FRAME_DOWNLINK_DATA ||
	     common.type == RB_FRAME_REPAIR_DATA) && core->slave_active) {
		struct rb_data_frame data;
		if (rb_data_decode(event->wire, event->wire_len, &data) != 0 ||
		    data.common.master_session != core->config.master_session ||
		    data.common.lease_id != 0u ||
		    data.stream_epoch != core->slave_downlink_epoch) {
			return;
		}
		if (rb_rx_window_insert(&core->slave_rx_window, data.sequence,
					data.payload, data.payload_len) != 0) {
			return;
		}
		for (;;) {
			uint8_t payload[RB_PACKET_DATA_MAX];
			uint32_t sequence;
			size_t payload_len;
			if (rb_rx_window_pop(&core->slave_rx_window, &sequence,
						 payload, sizeof(payload), &payload_len) != 0) {
				break;
			}
			(void)queue_append(core->slave_uart_queue, &core->slave_uart_head,
					    &core->slave_uart_count, payload, payload_len,
					    &core->queue_drop_bytes);
		}
	}
}

static int slave_next_action(struct rb_scheduler_core *core, uint64_t now_us,
				     struct rb_scheduler_action *action)
{
	if (core->slave_active && rb_slave_lease_tick(&core->slave_lease, now_us)) {
		core->slave_active = false;
		core->slave_node_id = 0u;
		core->slave_lease_id = 0u;
		core->slave_ack_pending = false;
		core->slave_ack_ready = false;
		action->type = RB_ACTION_ENTER_GROUP_RX;
		action->pipe = 0u;
		action->due_tick = now_us;
		return 0;
	}
	if (core->slave_assign_rx_active &&
	    now_us >= core->slave_assign_rx_deadline) {
		core->slave_assign_rx_active = false;
		action->type = RB_ACTION_ENTER_GROUP_RX;
		action->pipe = 0u;
		action->due_tick = now_us;
		return 0;
	}
	if (core->slave_ack_pending) {
		core->slave_ack_pending = false;
		action->type = RB_ACTION_QUEUE_ACK;
		action->pipe = core->slave_node_id;
		action->wire_len = core->slave_ack_wire_len;
		memcpy(action->wire, core->slave_ack_wire, action->wire_len);
		return 0;
	}
	if (core->slave_group_rx_pending) {
		core->slave_group_rx_pending = false;
		action->type = RB_ACTION_ENTER_GROUP_RX;
		action->pipe = 0u;
		action->due_tick = now_us;
		return 0;
	}
	if (core->slave_hello_pending && now_us >= core->slave_hello_due_tick) {
		struct rb_hello hello = {
			.common = RB_COMMON_INIT(RB_FRAME_HELLO,
						 core->config.master_session, 0, 0),
			.device_id = core->config.device_id,
			.discovery_nonce = core->slave_discovery_nonce,
		};
		core->slave_hello_pending = false;
		action->type = RB_ACTION_SEND_HELLO;
		action->pipe = 0;
		action->no_ack = true;
		if (rb_hello_encode(&hello, action->wire, sizeof(action->wire),
				    &action->wire_len) != 0) {
			return -EINVAL;
		}
		core->transaction_in_flight = true;
		core->in_flight_type = action->type;
		return 0;
	}
	if (core->transaction_in_flight) {
		return -EAGAIN;
	}
	if (core->slave_assign_rx_pending &&
	    now_us >= core->slave_assign_rx_deadline) {
		core->slave_assign_rx_pending = false;
		core->slave_assign_rx_active = true;
		core->slave_assign_rx_deadline = now_us +
			(core->config.assignment_window_us ?
			 core->config.assignment_window_us : 6000u);
		action->type = RB_ACTION_ENTER_ASSIGN_RX;
		action->pipe = 0;
		action->due_tick = now_us;
		return 0;
	}
	return -EAGAIN;
}

void rb_scheduler_init(struct rb_scheduler_core *core,
			 const struct rb_scheduler_config *config)
{
	if (core == NULL) {
		return;
	}
	memset(core, 0, sizeof(*core));
	if (config != NULL) {
		core->config = *config;
	}
	if (core->config.sync_interval_us == 0u) {
		core->config.sync_interval_us = RB_SCHEDULER_DEFAULT_SYNC_US;
	}
	if (core->config.aggregation_timeout_us == 0u) {
		core->config.aggregation_timeout_us = RB_SCHEDULER_DEFAULT_AGGREGATION_US;
	}
	if (core->config.max_idle_poll_us == 0u) {
		core->config.max_idle_poll_us = 5000u;
	}
	if (core->config.lease_timeout_us == 0u) {
		core->config.lease_timeout_us = RB_SCHEDULER_DEFAULT_LEASE_US;
	}
	if (core->config.assignment_window_us == 0u) {
		core->config.assignment_window_us = 6000u;
	}
	if (core->config.response_slot_count == 0u) {
		core->config.response_slot_count = RB_SCHEDULER_DEFAULT_RESPONSE_SLOTS;
	}
	if (core->config.response_slot_us == 0u) {
		core->config.response_slot_us = RB_SCHEDULER_DEFAULT_RESPONSE_SLOT_US;
	}
	if (core->config.master_session == 0u) {
		core->config.master_session = 1u;
	}
	/* Actions remain in flight until a TX event or the runtime's explicit
	 * action-failure callback clears the transaction gate. */
	core->free_slots = RB_SCHEDULER_MAX_PEERS;
	core->downlink_epoch = RB_SCHEDULER_DEFAULT_DOWNLINK_EPOCH;
	core->uplink_epoch = RB_SCHEDULER_DEFAULT_UPLINK_EPOCH;
	core->next_downlink_sequence = 1u;
	core->slave_uplink_sequence = 1u;
	core->next_lease_id = 1u;
	core->next_stream_epoch = 1u;
	core->next_sync_tick = core->config.sync_interval_us;
	rb_membership_init(&core->membership, core->config.master_session,
			   core->config.lease_timeout_us);
	rb_tx_window_init(&core->downlink_history,
			  (uint16_t)core->downlink_epoch, core->downlink_slots,
			  RB_LINK_WINDOW_SIZE);
	rb_rx_window_init(&core->slave_rx_window, 1u, 1u,
				  core->slave_rx_slots, RB_LINK_WINDOW_SIZE);
	rb_slave_lease_init(&core->slave_lease, core->config.lease_timeout_us);
}

int rb_scheduler_add_active_peer(struct rb_scheduler_core *core,
				 uint8_t node_id, uint64_t device_id,
				 uint16_t lease_id, uint64_t now_us)
{
	struct rb_scheduler_peer_runtime *runtime;
	int ret;

	if (core == NULL || !node_valid(node_id) || device_id == 0u ||
		lease_id == 0u) {
		return -EINVAL;
	}
	ret = rb_membership_begin_assign(&core->membership, device_id, now_us,
					node_id, lease_id);
	if (ret < 0) {
		return ret;
	}
	ret = rb_membership_assign_acked(&core->membership, node_id, now_us);
	if (ret != 0) {
		return ret;
	}
	runtime = peer_runtime(core, node_id);
	runtime->next_poll_due_us = (uint32_t)now_us;
	/* An active peer participates in continuous round-robin service until an
	 * idle observation explicitly applies exponential backoff. */
	runtime->poll_interval_us = 0u;
	runtime->poll_sequence = 1u;
	/* A caller may add peers while an action is in flight; the just-added peer
	 * is due immediately and participates in the next round. */
	core->free_slots = rb_membership_free_slots(&core->membership);
	return 0;
}

int rb_scheduler_queue_downlink(struct rb_scheduler_core *core,
				const uint8_t *data, size_t len)
{
	if (core == NULL || (data == NULL && len != 0u) || len == 0u ||
		len > RB_PACKET_DATA_MAX) {
		return -EINVAL;
	}
	{
		struct rb_data_frame frame = {
			.common = RB_COMMON_INIT(RB_FRAME_DOWNLINK_DATA,
						 core->config.master_session, 0, 0),
			.stream_epoch = (uint16_t)core->downlink_epoch,
			.sequence = core->next_downlink_sequence,
			.payload = data,
			.payload_len = len,
		};
		if (rb_data_encode(frame.common.type, frame.common.master_session,
				   frame.common.lease_id, frame.stream_epoch,
				   frame.sequence, frame.payload, frame.payload_len,
				   core->queued_wire, sizeof(core->queued_wire),
				   &core->queued_wire_len) != 0) {
			return -EINVAL;
		}
	}
	core->queued_broadcast = true;
	return 0;
}

size_t rb_scheduler_uart_write(struct rb_scheduler_core *core,
			       const uint8_t *data, size_t len, uint64_t now_us)
{
	size_t accepted = 0;

	if (core == NULL || data == NULL || len == 0u) {
		return 0u;
	}
	if (!core->config.master) {
		return queue_append(core->slave_uplink_queue, &core->slave_uplink_head,
					&core->slave_uplink_count, data, len,
					&core->queue_drop_bytes);
	}
	accepted = queue_append(core->uart_queue, &core->uart_head,
				&core->uart_count, data, len,
				&core->queue_drop_bytes);
	if (core->first_uart_tick == 0u) {
		core->first_uart_tick = now_us;
	}
	return accepted;
}

size_t rb_scheduler_uart_available(const struct rb_scheduler_core *core)
{
	size_t queued;

	if (core == NULL) {
		return 0u;
	}
	queued = core->config.master ? core->uart_count : core->slave_uplink_count;
	return RB_SCHEDULER_UART_QUEUE_SIZE - queued;
}

int rb_scheduler_next_action(struct rb_scheduler_core *core, uint64_t now_us,
			     struct rb_scheduler_action *action)
{
	uint8_t free_slots_before_tick;

	if (core == NULL || action == NULL) {
		return -EINVAL;
	}
	action_clear(action);
	/* Reserved/active lease expiry is handled here for real elapsed time.  A
	 * synthetic native tick can be earlier than the last successful poll; do
	 * not expire a peer on unsigned underflow. */
	free_slots_before_tick = rb_membership_free_slots(&core->membership);
	rb_membership_tick(&core->membership, now_us);
	if (core->action_retry_not_before_us != 0u) {
		if (now_us < core->action_retry_not_before_us) {
			return -EAGAIN;
		}
		core->action_retry_not_before_us = 0u;
	}
	if (!core->config.master) {
		return slave_next_action(core, now_us, action);
	}
	if (rb_membership_free_slots(&core->membership) != free_slots_before_tick) {
		core->free_slots = rb_membership_free_slots(&core->membership);
	}
	if (core->candidate_next == core->candidate_count) {
		core->candidate_next = 0u;
		core->candidate_count = 0u;
	}
	if (core->transaction_in_flight) {
		return -EAGAIN;
	}
	if (now_us >= core->next_sync_tick) {
		return build_sync(core, now_us, action);
	}
	if (core->discovery_rx_pending) {
		core->discovery_rx_pending = false;
		action->type = RB_ACTION_ENTER_DISCOVERY_RX;
		action->pipe = 0;
		action->no_ack = true;
		action->due_tick = now_us;
		return 0;
	}
	while (core->candidate_next < core->candidate_count &&
	       now_us >= core->response_window_deadline) {
		struct rb_scheduler_candidate *candidate =
			&core->candidates[core->candidate_next];
		uint8_t node_id = 0;
		for (uint8_t i = 1; i <= RB_SCHEDULER_MAX_PEERS; i++) {
			if (rb_membership_peer(&core->membership, i)->state == RB_PEER_FREE) {
				node_id = i;
				break;
			}
		}
		if (node_id == 0u) {
			break;
		}
		uint16_t lease = next_nonzero_u16(&core->next_lease_id);
		if (rb_membership_begin_assign(&core->membership, candidate->device_id,
						now_us, node_id, lease) < 0) {
			break;
		}
		memset(peer_runtime(core, node_id), 0, sizeof(core->peer[0]));
		{
			struct rb_assign frame = {
				.common = RB_COMMON_INIT(RB_FRAME_ASSIGN,
							 core->config.master_session, lease, 0),
				.target_device_id = candidate->device_id,
				.node_id = node_id,
				.pipe = node_id,
				.downlink_epoch = (uint16_t)core->downlink_epoch,
				.uplink_epoch = (uint16_t)core->uplink_epoch,
				.max_payload = RB_PACKET_DATA_MAX,
				.link_window = RB_LINK_WINDOW_SIZE,
				.retry_count = 3,
				.lease_timeout_us = core->config.lease_timeout_us,
			};
			if (rb_assign_encode(&frame, action->wire, sizeof(action->wire),
					     &action->wire_len) != 0) {
				return -EINVAL;
			}
		}
		candidate->queued = false;
		core->candidate_next++;
		core->free_slots = rb_membership_free_slots(&core->membership);
		action->type = RB_ACTION_SEND_ASSIGN;
		action->node_id = node_id;
		action->target_device_id = candidate->device_id;
		action->pipe = node_id;
		action->due_tick = now_us;
		core->transaction_in_flight = true;
		core->in_flight_node = node_id;
		core->in_flight_pipe = node_id;
		core->in_flight_type = action->type;
		return 0;
	}
	for (uint8_t i = 0; i < RB_SCHEDULER_MAX_PEERS; i++) {
		uint8_t node_id = (uint8_t)(((core->poll_cursor + i) %
					 RB_SCHEDULER_MAX_PEERS) + 1u);
		struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);
		if (runtime == NULL || !peer_schedulable(core, node_id) ||
			(!runtime->repair_pending && !runtime->skip_pending)) {
			continue;
		}
	core->poll_cursor = node_id % RB_SCHEDULER_MAX_PEERS;
		if (build_repair_or_skip(core, node_id, now_us, action) == 0) {
			return 0;
		}
	}
	if (core->queued_broadcast) {
		return build_broadcast(core, now_us, action);
	}
	if (core->uart_count != 0u &&
		(core->uart_count >= RB_PACKET_DATA_MAX ||
		 (core->first_uart_tick != 0u && now_us - core->first_uart_tick >=
		  core->config.aggregation_timeout_us))) {
		return build_broadcast(core, now_us, action);
	}
	for (uint8_t i = 0; i < RB_SCHEDULER_MAX_PEERS; i++) {
		uint8_t node_id = (uint8_t)(((core->poll_cursor + i) %
					 RB_SCHEDULER_MAX_PEERS) + 1u);
		struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);
		int ret;

		if (runtime == NULL || !peer_schedulable(core, node_id)) {
			continue;
		}
		if (runtime->poll_retry_backoff &&
		    now_us >= (uint64_t)runtime->next_poll_due_us) {
			runtime->poll_retry_backoff = false;
		}
		if (now_us < (uint64_t)runtime->next_poll_due_us &&
		    (!runtime->data_ready || runtime->poll_retry_backoff)) {
			continue;
		}
		core->poll_cursor = node_id % RB_SCHEDULER_MAX_PEERS;
		ret = build_poll(core, node_id, now_us, action);
		if (ret == 0) {
			return 0;
		}
	}
	return -EAGAIN;
}

void rb_scheduler_action_failed(struct rb_scheduler_core *core,
				const struct rb_scheduler_action *action,
				uint64_t now_us)
{
	struct rb_scheduler_peer_runtime *runtime;
	uint64_t retry_due;
	enum rb_scheduler_action_type type;
	bool failed_in_flight;
	uint8_t node_id;

	if (core == NULL || action == NULL || action->type == RB_ACTION_NONE) {
		return;
	}
	type = action->type;
	failed_in_flight = core->transaction_in_flight &&
		core->in_flight_type == type;
	if (action_uses_transaction_gate(type) && !failed_in_flight) {
		return;
	}
	core->action_retry_not_before_us =
		now_us + RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
	node_id = failed_in_flight ? core->in_flight_node : action->node_id;
	retry_due = now_us + RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
	switch (type) {
	case RB_ACTION_SEND_SYNC_DISCOVERY:
		core->next_sync_tick = retry_due;
		core->discovery_rx_pending = false;
		core->response_window_deadline = 0u;
		break;
	case RB_ACTION_ENTER_DISCOVERY_RX:
		core->discovery_rx_pending = true;
		break;
	case RB_ACTION_SEND_POLL:
		runtime = peer_runtime(core, node_id);
		if (runtime != NULL) {
			runtime->next_poll_due_us = (uint32_t)retry_due;
			runtime->data_ready = core->in_flight_poll_data_ready;
			runtime->poll_retry_backoff = core->in_flight_poll_data_ready;
		}
		break;
	case RB_ACTION_SEND_HELLO:
		core->slave_hello_pending = true;
		core->slave_hello_due_tick = retry_due;
		break;
	case RB_ACTION_SEND_REPAIR:
		runtime = peer_runtime(core, node_id);
		if (runtime != NULL) {
			runtime->repair_pending = true;
		}
		break;
	case RB_ACTION_SEND_SKIP_TO:
		runtime = peer_runtime(core, node_id);
		if (runtime != NULL) {
			runtime->skip_pending = true;
		}
		break;
	case RB_ACTION_SEND_DOWNLINK_BROADCAST:
		memcpy(core->queued_wire, action->wire, action->wire_len);
		core->queued_wire_len = action->wire_len;
		core->queued_broadcast = true;
		break;
	case RB_ACTION_SEND_ASSIGN:
		{
			const struct rb_membership_peer *peer =
				rb_membership_peer(&core->membership, node_id);
			uint64_t device_id = peer == NULL ? 0u : peer->device_id;

			(void)rb_membership_abort_assign(&core->membership, node_id);
			runtime = peer_runtime(core, node_id);
			if (runtime != NULL) {
				memset(runtime, 0, sizeof(*runtime));
			}
			for (uint8_t i = 0u; i < core->candidate_count; i++) {
				if (core->candidates[i].device_id != device_id) {
					continue;
				}
				core->candidates[i].queued = true;
				if (i < core->candidate_next) {
					core->candidate_next = i;
				}
				break;
			}
			core->free_slots = rb_membership_free_slots(&core->membership);
		}
		break;
	case RB_ACTION_QUEUE_ACK:
		core->slave_ack_pending = true;
		break;
	case RB_ACTION_ENTER_ASSIGN_RX:
		core->slave_assign_rx_active = false;
		core->slave_assign_rx_pending = true;
		core->slave_assign_rx_deadline = retry_due;
		break;
	case RB_ACTION_ENTER_GROUP_RX:
		core->slave_group_rx_pending = true;
		break;
	default:
		break;
	}
	if (failed_in_flight) {
		core->transaction_in_flight = false;
		core->in_flight_node = 0u;
		core->in_flight_pipe = 0u;
		core->in_flight_type = RB_ACTION_NONE;
		core->in_flight_poll_data_ready = false;
	}
}

void rb_scheduler_on_radio_event(struct rb_scheduler_core *core,
				 const struct rb_radio_event_view *event,
				 uint64_t now_us)
{
	if (core == NULL || event == NULL) {
		return;
	}
	if (!core->config.master) {
		slave_handle_rx(core, event, now_us);
		return;
	}
	if (event->type == RB_EVENT_VIEW_TX_SUCCESS ||
	    event->type == RB_EVENT_VIEW_TX_FAILED) {
		if (event->type == RB_EVENT_VIEW_TX_SUCCESS &&
		    core->in_flight_type == RB_ACTION_SEND_ASSIGN) {
			(void)rb_membership_assign_acked(&core->membership,
						 core->in_flight_node, now_us);
		}
		if (event->type == RB_EVENT_VIEW_TX_FAILED) {
			(void)rb_membership_note_failure(&core->membership,
						core->in_flight_node, now_us);
		}
		core->transaction_in_flight = false;
		return;
	}
	if (event->type == RB_EVENT_VIEW_RX_RECEIVED && event->wire != NULL) {
		struct rb_hello hello;
		if (core->free_slots != 0u &&
			rb_hello_decode(event->wire, event->wire_len, &hello) == 0 &&
			hello.common.master_session == core->config.master_session) {
			bool duplicate = false;

			if (membership_has_device(core, hello.device_id)) {
				return;
			}
			for (uint8_t i = 0; i < core->candidate_count; i++) {
				if (core->candidates[i].device_id == hello.device_id) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate && core->candidate_count < RB_SCHEDULER_MAX_PEERS) {
				struct rb_scheduler_candidate *candidate =
					&core->candidates[core->candidate_count++];
				candidate->device_id = hello.device_id;
				candidate->capabilities = hello.capabilities;
				candidate->discovery_nonce = hello.discovery_nonce;
				candidate->queued = true;
			}
			return;
		}
		struct rb_ack_uplink ack;
		uint8_t node_id = event->pipe;
		int ack_ret = rb_ack_uplink_decode(event->wire, event->wire_len, &ack);
		if (ack_ret == 0 && !node_valid(node_id) &&
			node_valid(ack.common.source_node)) {
			node_id = ack.common.source_node;
		}
		if (ack_ret == 0 && node_valid(node_id)) {
			struct rb_scheduler_peer_runtime *runtime =
				peer_runtime(core, node_id);
			bool duplicate = false;
			if (ack.common.master_session != core->config.master_session ||
			    ack.common.lease_id != rb_membership_peer(&core->membership,
								      node_id)->lease_id ||
			    ack.uplink_epoch != (uint16_t)core->uplink_epoch ||
			    ack.downlink_epoch != (uint16_t)core->downlink_epoch ||
			    (ack.payload_len != 0u && ack.uplink_sequence == 0u)) {
				core->invalid_session_rx_count++;
				return;
			}
			/* Duplicate detection: if the uplink sequence is at or behind
			 * what we last acknowledged, this is a retransmit we already
			 * delivered — count it but still process the downlink ACK bitmap. */
			if (ack.payload_len != 0u && runtime->uplink_ack_base != 0u &&
			    (ack.uplink_sequence == runtime->uplink_ack_base ||
			     rb_seq_before(ack.uplink_sequence,
					   runtime->uplink_ack_base))) {
				duplicate = true;
				core->duplicate_rx_count++;
			}
			if (ack.payload_len != 0u) {
					rb_scheduler_mark_peer_data(core, node_id, now_us);
				} else {
					rb_scheduler_mark_peer_idle(core, node_id, now_us);
				}
			rb_membership_note_success(&core->membership, node_id, now_us);
			rb_scheduler_note_downlink_ack(core, node_id,
						       ack.downlink_ack_base,
						       ack.downlink_ack_bitmap);
			if (ack.payload_len != 0u && !duplicate) {
				runtime->uplink_ack_base = ack.uplink_sequence;
				(void)queue_append(core->master_rx_queue,
						   &core->master_rx_head,
						   &core->master_rx_count, ack.payload,
						   ack.payload_len,
						   &core->queue_drop_bytes);
			}
		}
	}
}

void rb_scheduler_note_downlink_ack(struct rb_scheduler_core *core,
					uint8_t node_id, uint32_t ack_base,
					uint64_t ack_bitmap)
{
	struct rb_scheduler_peer_runtime *runtime;
	uint32_t missing;

	if (core == NULL || !node_valid(node_id) || !peer_schedulable(core, node_id)) {
		return;
	}
	runtime = peer_runtime(core, node_id);
	/* Release confirmed TX history slots so the window does not stay full
	 * and force unnecessary SKIP_TO for every subsequent peer. */
	rb_tx_window_apply_ack(&core->downlink_history, ack_base, ack_bitmap);
	missing = ack_base + 1u;
	if (missing == core->next_downlink_sequence ||
		(ack_bitmap & UINT64_C(1)) != 0u) {
		return;
	}
	runtime->missing_sequence = missing;
	runtime->repair_pending = true;
	runtime->skip_pending = !rb_tx_window_contains(&core->downlink_history,
						       missing);
}

void rb_scheduler_mark_peer_idle(struct rb_scheduler_core *core,
					 uint8_t node_id, uint64_t now_us)
{
	struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);

	if (runtime == NULL) {
		return;
	}
	if (runtime->poll_interval_us == 0u) {
		runtime->poll_interval_us = RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
	}
	runtime->poll_interval_us *= 2u;
	if (runtime->poll_interval_us > idle_limit(core)) {
		runtime->poll_interval_us = idle_limit(core);
	}
	runtime->next_poll_due_us = (uint32_t)now_us + runtime->poll_interval_us;
}

void rb_scheduler_mark_peer_data(struct rb_scheduler_core *core,
					 uint8_t node_id, uint64_t now_us)
{
	struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);

	if (runtime == NULL) {
		return;
	}
	runtime->poll_interval_us = 0u;
	runtime->data_ready = true;
	runtime->next_poll_due_us = (uint32_t)now_us;
}

void rb_scheduler_mark_peer_suspect(struct rb_scheduler_core *core,
					    uint8_t node_id, uint64_t now_us)
{
	if (core == NULL) {
		return;
	}
	(void)rb_membership_note_failure(&core->membership, node_id, now_us);
	{
		struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);
		if (runtime != NULL) {
			runtime->poll_interval_us = idle_limit(core);
			runtime->next_poll_due_us = (uint32_t)now_us + runtime->poll_interval_us;
		}
	}
}

void rb_scheduler_set_sync_deadline(struct rb_scheduler_core *core,
					    uint64_t tick)
{
	if (core != NULL) {
		core->next_sync_tick = tick;
	}
}

void rb_scheduler_set_free_slots(struct rb_scheduler_core *core,
				 uint8_t free_slots)
{
	if (core != NULL) {
		core->free_slots = free_slots > RB_SCHEDULER_MAX_PEERS ?
			RB_SCHEDULER_MAX_PEERS : free_slots;
	}
}

void rb_scheduler_set_discovery_slot(struct rb_scheduler_core *core,
					 uint8_t slot)
{
	if (core == NULL) {
		return;
	}
	core->slave_discovery_slot = slot;
	core->slave_discovery_slot_valid = true;
}

uint32_t rb_scheduler_peer_poll_interval(const struct rb_scheduler_core *core,
						 uint8_t node_id)
{
	const struct rb_scheduler_peer_runtime *runtime =
		peer_runtime_const(core, node_id);

	return runtime == NULL ? 0u : runtime->poll_interval_us;
}

uint8_t rb_scheduler_active_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : rb_membership_active_count(&core->membership);
}

uint64_t rb_scheduler_queue_drop_bytes(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->queue_drop_bytes;
}

bool rb_scheduler_take_evicted(struct rb_scheduler_core *core, uint32_t *sequence)
{
	if (core == NULL || sequence == NULL) {
		return false;
	}
	return rb_tx_window_take_evicted(&core->downlink_history, sequence);
}

uint64_t rb_scheduler_duplicate_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->duplicate_rx_count;
}

uint64_t rb_scheduler_invalid_session_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->invalid_session_rx_count;
}

uint32_t rb_scheduler_session(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->config.master_session;
}

int rb_scheduler_reset_session(struct rb_scheduler_core *core,
			       uint32_t master_session)
{
	if (core == NULL || master_session == 0u) {
		return -EINVAL;
	}
	core->config.master_session = master_session;
	rb_membership_init(&core->membership, master_session,
			   core->config.lease_timeout_us);
	memset(core->peer, 0, sizeof(core->peer));
	memset(core->candidates, 0, sizeof(core->candidates));
	core->candidate_count = 0u;
	core->candidate_next = 0u;
	core->poll_cursor = 0u;
	core->free_slots = RB_SCHEDULER_MAX_PEERS;
	core->discovery_rx_pending = false;
	core->sync_in_flight = false;
	core->next_downlink_sequence = 1u;
	core->slave_uplink_sequence = 1u;
	core->downlink_epoch = next_nonzero_u16(&core->next_stream_epoch);
	core->uplink_epoch = next_nonzero_u16(&core->next_stream_epoch);
	core->transaction_in_flight = false;
	core->action_retry_not_before_us = 0u;
	core->in_flight_node = 0u;
	core->in_flight_pipe = 0u;
	core->in_flight_type = RB_ACTION_NONE;
	core->queued_broadcast = false;
	clear_uart(core);
	core->master_rx_head = 0u;
	core->master_rx_count = 0u;
	core->slave_active = false;
	rb_slave_lease_init(&core->slave_lease, core->config.lease_timeout_us);
	core->slave_hello_pending = false;
	core->slave_discovery_nonce = 0u;
	core->slave_discovery_slot = 0u;
	core->slave_discovery_slot_valid = false;
	core->slave_assign_rx_pending = false;
	core->slave_assign_rx_active = false;
	core->slave_group_rx_pending = false;
	core->slave_ack_pending = false;
	core->slave_ack_ready = false;
	core->slave_ack_wire_len = 0u;
	core->slave_credit_bytes = 0u;
	core->slave_last_poll_sequence = 0u;
	core->slave_uplink_inflight = false;
	core->slave_uplink_inflight_len = 0u;
	core->slave_uplink_head = 0u;
	core->slave_uplink_count = 0u;
	core->slave_uart_head = 0u;
	core->slave_uart_count = 0u;
	core->next_sync_tick = core->config.sync_interval_us;
	rb_rx_window_reset_epoch(&core->slave_rx_window, 1u, 1u);
	rb_tx_window_reset_epoch(&core->downlink_history,
				 (uint16_t)core->downlink_epoch);
	return 0;
}

size_t rb_scheduler_slave_read_uart(struct rb_scheduler_core *core,
					uint8_t *data, size_t max_len)
{
	size_t n;

	if (core == NULL || data == NULL || max_len == 0u) {
		return 0u;
	}
	n = core->slave_uart_count < max_len ? core->slave_uart_count : max_len;
	for (size_t i = 0; i < n; i++) {
		data[i] = core->slave_uart_queue[(core->slave_uart_head + i) %
					 RB_SCHEDULER_UART_QUEUE_SIZE];
	}
	core->slave_uart_head = (uint16_t)((core->slave_uart_head + n) %
					 RB_SCHEDULER_UART_QUEUE_SIZE);
	core->slave_uart_count = (uint16_t)(core->slave_uart_count - n);
	return n;
}

size_t rb_scheduler_master_read_uart(struct rb_scheduler_core *core,
					 uint8_t *data, size_t max_len)
{
	size_t n;
	if (core == NULL || data == NULL || max_len == 0u) {
		return 0u;
	}
	n = core->master_rx_count < max_len ? core->master_rx_count : max_len;
	for (size_t i = 0; i < n; i++) {
		data[i] = core->master_rx_queue[(core->master_rx_head + i) %
			RB_SCHEDULER_UART_QUEUE_SIZE];
	}
	queue_discard(&core->master_rx_head, &core->master_rx_count, n);
	return n;
}
