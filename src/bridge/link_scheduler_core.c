#include "link_scheduler_core.h"

#include <errno.h>
#include <string.h>

#define RB_SCHEDULER_DEFAULT_AGGREGATION_US 1000u
#define RB_SCHEDULER_DEFAULT_SYNC_US 100000u
#define RB_SCHEDULER_DEFAULT_LEASE_US 100000u

static bool node_valid(uint8_t node_id)
{
	return node_id >= 1u && node_id <= RB_SCHEDULER_MAX_PEERS;
}

static bool sequence_before(uint32_t first, uint32_t second)
{
	return (int32_t)(first - second) < 0;
}

static struct rb_scheduler_peer_runtime *peer_runtime(
	struct rb_scheduler_core *core, uint8_t node_id)
{
	return core == NULL || !node_valid(node_id) ? NULL :
		&core->peer[node_id - 1u];
}

static const struct rb_scheduler_peer_runtime *peer_runtime_const(
	const struct rb_scheduler_core *core, uint8_t node_id)
{
	return core == NULL || !node_valid(node_id) ? NULL :
		&core->peer[node_id - 1u];
}

static uint32_t idle_limit(const struct rb_scheduler_core *core)
{
	return core->config.max_idle_poll_us == 0u ? 5000u :
		core->config.max_idle_poll_us;
}

static void action_clear(struct rb_scheduler_action *action)
{
	memset(action, 0, sizeof(*action));
	action->type = RB_ACTION_NONE;
}

static size_t queue_peek(const uint8_t *queue, uint16_t head, uint16_t count,
			 uint8_t *out, size_t max_len)
{
	size_t length = count < max_len ? count : max_len;

	for (size_t i = 0u; i < length; i++) {
		out[i] = queue[(head + i) % RB_SCHEDULER_UART_QUEUE_SIZE];
	}
	return length;
}

static void queue_discard(uint16_t *head, uint16_t *count, size_t len)
{
	if (len > *count) {
		len = *count;
	}
	*head = (uint16_t)((*head + len) % RB_SCHEDULER_UART_QUEUE_SIZE);
	*count = (uint16_t)(*count - len);
}

static size_t queue_append(uint8_t *queue, uint16_t *head, uint16_t *count,
			   const uint8_t *data, size_t len,
			   uint64_t *dropped_bytes)
{
	size_t original_len = len;
	size_t overflow;

	if (queue == NULL || head == NULL || count == NULL || data == NULL || len == 0u) {
		return 0u;
	}
	if (len >= RB_SCHEDULER_UART_QUEUE_SIZE) {
		size_t skipped = len - RB_SCHEDULER_UART_QUEUE_SIZE;

		if (dropped_bytes != NULL) {
			*dropped_bytes += *count + skipped;
		}
		memcpy(queue, data + skipped, RB_SCHEDULER_UART_QUEUE_SIZE);
		*head = 0u;
		*count = RB_SCHEDULER_UART_QUEUE_SIZE;
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
	for (size_t i = 0u; i < len; i++) {
		queue[(*head + *count) % RB_SCHEDULER_UART_QUEUE_SIZE] = data[i];
		(*count)++;
	}
	return original_len;
}

static size_t master_uart_peek(const struct rb_scheduler_core *core,
			       uint8_t *out, size_t max_len)
{
	return queue_peek(core->uart_queue, core->uart_head, core->uart_count,
			  out, max_len);
}

static void master_uart_discard(struct rb_scheduler_core *core, size_t len)
{
	queue_discard(&core->uart_head, &core->uart_count, len);
	if (core->uart_count == 0u) {
		core->first_uart_tick = 0u;
	}
}

static uint32_t next_nonzero_sequence(uint32_t *sequence)
{
	uint32_t value = *sequence;

	if (value == 0u) {
		value = 1u;
	}
	*sequence = value + 1u;
	if (*sequence == 0u) {
		*sequence = 1u;
	}
	return value;
}

static void slave_prepare_ack(struct rb_scheduler_core *core)
{
	uint8_t payload[RB_ACK_UPLINK_PAYLOAD_MAX];
	size_t payload_len = 0u;
	uint32_t payload_sequence = 0u;
	struct rb_ack_uplink ack;

	if (core->config.master_session == 0u || core->config.slave_session == 0u ||
	    !node_valid(core->config.node_id)) {
		return;
	}
	if (core->slave_uplink_inflight) {
		payload_len = queue_peek(core->slave_uplink_queue,
					 core->slave_uplink_head,
					 (uint16_t)core->slave_uplink_inflight_len,
					 payload, sizeof(payload));
		payload_sequence = core->slave_uplink_inflight_sequence;
	} else if (core->slave_uplink_count != 0u && core->slave_credit_bytes != 0u) {
		size_t limit = core->slave_credit_bytes < sizeof(payload) ?
			core->slave_credit_bytes : sizeof(payload);

		payload_len = queue_peek(core->slave_uplink_queue,
					 core->slave_uplink_head,
					 core->slave_uplink_count,
					 payload, limit);
		if (payload_len != 0u) {
			payload_sequence = next_nonzero_sequence(
				&core->slave_uplink_sequence);
			core->slave_uplink_inflight = true;
			core->slave_uplink_inflight_sequence = payload_sequence;
			core->slave_uplink_inflight_len = payload_len;
		}
	}
	ack = (struct rb_ack_uplink){
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK,
					 core->config.master_session,
					 core->config.node_id),
		.slave_session = core->config.slave_session,
		.uplink_sequence = payload_sequence,
		.drop_count = (uint32_t)core->queue_drop_bytes,
		.payload = payload_len == 0u ? NULL : payload,
		.payload_len = payload_len,
	};
	if (rb_ack_uplink_encode(&ack, core->slave_ack_wire,
				 sizeof(core->slave_ack_wire),
				 &core->slave_ack_wire_len) == 0) {
		core->slave_ack_pending = true;
	}
}

static int build_poll(struct rb_scheduler_core *core, uint8_t node_id,
		      uint64_t now_us, bool inactive,
		      struct rb_scheduler_action *action)
{
	const struct rb_fixed_peer *fixed =
		rb_fixed_peer_get(&core->fixed_peers, node_id);
	struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);
	struct rb_poll poll;

	if (fixed == NULL || runtime == NULL) {
		return -EINVAL;
	}
	poll = (struct rb_poll){
		.common = RB_COMMON_INIT(RB_FRAME_POLL,
					 core->config.master_session, 0u),
		.known_slave_session = fixed->active ? fixed->slave_session : 0u,
		.uplink_ack_sequence = fixed->active ?
			runtime->uplink_ack_sequence : 0u,
		.next_credit_bytes = RB_ACK_UPLINK_PAYLOAD_MAX,
		.poll_sequence = runtime->poll_sequence++,
	};
	if (rb_poll_encode(&poll, action->wire, sizeof(action->wire),
			   &action->wire_len) != 0) {
		return -EINVAL;
	}
	action->type = RB_ACTION_SEND_POLL;
	action->node_id = node_id;
	action->pipe = node_id;
	action->due_tick = now_us;
	core->transaction_in_flight = true;
	core->in_flight_node = node_id;
	core->in_flight_type = action->type;
	if (inactive) {
		core->next_probe_due_us = now_us + RB_SCHEDULER_INACTIVE_PROBE_US;
	} else {
		runtime->next_poll_due_us = now_us + runtime->poll_interval_us;
		runtime->data_ready = false;
	}
	return 0;
}

static int build_sync(struct rb_scheduler_core *core, uint64_t now_us,
		      struct rb_scheduler_action *action)
{
	uint32_t interval = core->config.sync_interval_us == 0u ?
		RB_SCHEDULER_DEFAULT_SYNC_US : core->config.sync_interval_us;
	struct rb_sync_frame sync = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC,
					 core->config.master_session, 0u),
		.group_id = core->config.group_id,
		.master_id = core->config.local_device_id,
		.sync_sequence = (uint32_t)(now_us / interval) + 1u,
		.sync_interval_us = interval,
		.next_pps_utc_seconds = core->time_publication.next_pps_utc_seconds,
		.time_quality = core->time_publication.time_quality,
	};

	if (rb_sync_encode(&sync, action->wire, sizeof(action->wire),
			   &action->wire_len) != 0) {
		return -EINVAL;
	}
	action->type = RB_ACTION_SEND_SYNC;
	action->pipe = 0u;
	action->no_ack = true;
	action->due_tick = now_us;
	core->next_sync_tick = now_us + interval;
	core->probe_preceded_overdue_sync = false;
	core->transaction_in_flight = true;
	core->in_flight_node = 0u;
	core->in_flight_type = action->type;
	return 0;
}

static int build_broadcast(struct rb_scheduler_core *core, uint64_t now_us,
			   struct rb_scheduler_action *action)
{
	uint8_t payload[RB_PACKET_DATA_MAX];
	size_t payload_len;

	if (core->queued_broadcast) {
		memcpy(action->wire, core->queued_wire, core->queued_wire_len);
		action->wire_len = core->queued_wire_len;
		core->queued_broadcast = false;
	} else {
		uint32_t sequence = next_nonzero_sequence(&core->next_downlink_sequence);

		payload_len = master_uart_peek(core, payload, sizeof(payload));
		if (payload_len == 0u) {
			return -EAGAIN;
		}
		if (rb_downlink_encode(core->config.master_session, sequence,
				       payload, payload_len, action->wire,
				       sizeof(action->wire), &action->wire_len) != 0) {
			return -EINVAL;
		}
		master_uart_discard(core, payload_len);
	}
	action->type = RB_ACTION_SEND_DOWNLINK_BROADCAST;
	action->pipe = 0u;
	action->no_ack = true;
	action->due_tick = now_us;
	core->transaction_in_flight = true;
	core->in_flight_node = 0u;
	core->in_flight_type = action->type;
	return 0;
}

static int next_inactive_node(struct rb_scheduler_core *core)
{
	for (uint8_t offset = 0u; offset < RB_SCHEDULER_MAX_PEERS; offset++) {
		uint8_t node_id = (uint8_t)(
			((core->probe_cursor + offset) % RB_SCHEDULER_MAX_PEERS) + 1u);
		const struct rb_fixed_peer *peer =
			rb_fixed_peer_get(&core->fixed_peers, node_id);

		if (peer != NULL && !peer->active) {
			core->probe_cursor = node_id % RB_SCHEDULER_MAX_PEERS;
			return node_id;
		}
	}
	return -ENOENT;
}

static int slave_next_action(struct rb_scheduler_core *core, uint64_t now_us,
			     struct rb_scheduler_action *action)
{
	if (core->slave_active && now_us >= core->slave_last_poll_us &&
	    now_us - core->slave_last_poll_us >= core->config.lease_timeout_us) {
		core->slave_active = false;
	}
	if (!core->slave_ack_pending) {
		return -EAGAIN;
	}
	action->type = RB_ACTION_QUEUE_ACK;
	action->node_id = core->config.node_id;
	action->pipe = core->config.node_id;
	action->replace_ack = core->slave_ack_replace;
	memcpy(action->wire, core->slave_ack_wire, core->slave_ack_wire_len);
	action->wire_len = core->slave_ack_wire_len;
	core->slave_ack_pending = false;
	core->slave_ack_replace = false;
	return 0;
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
		core->config.aggregation_timeout_us =
			RB_SCHEDULER_DEFAULT_AGGREGATION_US;
	}
	if (core->config.max_idle_poll_us == 0u) {
		core->config.max_idle_poll_us = 5000u;
	}
	if (core->config.lease_timeout_us == 0u) {
		core->config.lease_timeout_us = RB_SCHEDULER_DEFAULT_LEASE_US;
	}
	core->next_downlink_sequence = 1u;
	core->slave_uplink_sequence = 1u;
	core->slave_credit_bytes = RB_ACK_UPLINK_PAYLOAD_MAX;
	core->next_sync_tick = core->config.sync_interval_us;
	core->slave_node_id = core->config.node_id;
	rb_fixed_peer_table_init(&core->fixed_peers,
				 core->config.lease_timeout_us);
	for (size_t i = 0u; i < RB_SCHEDULER_MAX_PEERS; i++) {
		rb_record_queue_init(&core->master_record_queue[i],
				     core->master_record_storage[i],
				     sizeof(core->master_record_storage[i]),
				     core->master_record_lengths[i],
				     RB_SCHEDULER_RECORD_LENGTH_CAPACITY);
	}
}

int rb_scheduler_set_time_publication(struct rb_scheduler_core *core,
				      int64_t next_pps_utc_seconds,
				      uint8_t time_quality)
{
	if (core == NULL || time_quality > RB_TIME_HOLDOVER) {
		return -EINVAL;
	}
	core->time_publication.next_pps_utc_seconds = next_pps_utc_seconds;
	core->time_publication.time_quality = time_quality;
	return 0;
}

int rb_scheduler_queue_downlink(struct rb_scheduler_core *core,
			const uint8_t *data, size_t len)
{
	uint32_t sequence;

	if (core == NULL || !core->config.master || data == NULL || len == 0u ||
	    len > RB_PACKET_DATA_MAX || core->queued_broadcast) {
		return -EINVAL;
	}
	sequence = next_nonzero_sequence(&core->next_downlink_sequence);
	if (rb_downlink_encode(core->config.master_session, sequence, data, len,
			       core->queued_wire, sizeof(core->queued_wire),
			       &core->queued_wire_len) != 0) {
		return -EINVAL;
	}
	core->queued_broadcast = true;
	return 0;
}

size_t rb_scheduler_uart_write(struct rb_scheduler_core *core,
			       const uint8_t *data, size_t len, uint64_t now_us)
{
	size_t accepted;

	if (core == NULL || data == NULL || len == 0u) {
		return 0u;
	}
	if (!core->config.master) {
		return queue_append(core->slave_uplink_queue,
				    &core->slave_uplink_head,
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
	size_t count;

	if (core == NULL) {
		return 0u;
	}
	count = core->config.master ? core->uart_count : core->slave_uplink_count;
	return RB_SCHEDULER_UART_QUEUE_SIZE - count;
}

int rb_scheduler_next_action(struct rb_scheduler_core *core, uint64_t now_us,
			     struct rb_scheduler_action *action)
{
	int inactive_node;
	bool sync_due;

	if (core == NULL || action == NULL) {
		return -EINVAL;
	}
	action_clear(action);
	if (core->action_retry_not_before_us != 0u) {
		if (now_us < core->action_retry_not_before_us) {
			return -EAGAIN;
		}
		core->action_retry_not_before_us = 0u;
	}
	if (!core->config.master) {
		return slave_next_action(core, now_us, action);
	}
	(void)rb_fixed_peer_expire(&core->fixed_peers, now_us);
	if (core->transaction_in_flight) {
		return -EAGAIN;
	}
	sync_due = now_us >= core->next_sync_tick;
	inactive_node = now_us >= core->next_probe_due_us ?
		next_inactive_node(core) : -ENOENT;
	if (inactive_node > 0 &&
	    (!sync_due || !core->probe_preceded_overdue_sync)) {
		if (sync_due) {
			core->probe_preceded_overdue_sync = true;
		}
		return build_poll(core, (uint8_t)inactive_node, now_us, true, action);
	}
	if (sync_due) {
		return build_sync(core, now_us, action);
	}
	if (core->queued_broadcast ||
	    (core->uart_count != 0u &&
	     (core->uart_count >= RB_PACKET_DATA_MAX ||
	      (core->first_uart_tick != 0u &&
	       now_us - core->first_uart_tick >=
		core->config.aggregation_timeout_us)))) {
		return build_broadcast(core, now_us, action);
	}
	for (uint8_t offset = 0u; offset < RB_SCHEDULER_MAX_PEERS; offset++) {
		uint8_t node_id = (uint8_t)(
			((core->poll_cursor + offset) % RB_SCHEDULER_MAX_PEERS) + 1u);
		const struct rb_fixed_peer *fixed =
			rb_fixed_peer_get(&core->fixed_peers, node_id);
		struct rb_scheduler_peer_runtime *runtime = peer_runtime(core, node_id);

		if (fixed == NULL || runtime == NULL || !fixed->active ||
		    (now_us < runtime->next_poll_due_us && !runtime->data_ready)) {
			continue;
		}
		core->poll_cursor = node_id % RB_SCHEDULER_MAX_PEERS;
		return build_poll(core, node_id, now_us, false, action);
	}
	return -EAGAIN;
}

void rb_scheduler_action_failed(struct rb_scheduler_core *core,
				const struct rb_scheduler_action *action,
				uint64_t now_us)
{
	if (core == NULL || action == NULL || action->type == RB_ACTION_NONE) {
		return;
	}
	core->action_retry_not_before_us =
		now_us + RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
	if (action->type == RB_ACTION_QUEUE_ACK) {
		core->slave_ack_pending = true;
		core->slave_ack_replace = action->replace_ack;
		return;
	}
	if (core->transaction_in_flight &&
	    core->in_flight_type == action->type) {
		if (action->type == RB_ACTION_SEND_POLL &&
		    node_valid(core->in_flight_node)) {
			struct rb_scheduler_peer_runtime *runtime =
				peer_runtime(core, core->in_flight_node);
			const struct rb_fixed_peer *fixed = rb_fixed_peer_get(
				&core->fixed_peers, core->in_flight_node);

			runtime->poll_failure_count++;
			if (fixed != NULL && fixed->active) {
				runtime->next_poll_due_us =
					now_us + RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
			} else {
				core->probe_cursor = core->in_flight_node - 1u;
				core->next_probe_due_us =
					now_us + RB_SCHEDULER_DEFAULT_IDLE_POLL_US;
			}
		}
		core->transaction_in_flight = false;
		core->in_flight_node = 0u;
		core->in_flight_type = RB_ACTION_NONE;
	}
}

static void slave_handle_sync(struct rb_scheduler_core *core,
			      const struct rb_radio_event_view *event)
{
	struct rb_sync_frame sync;

	if (event->pipe != 0u ||
	    rb_sync_decode(event->wire, event->wire_len, &sync) != 0 ||
	    sync.group_id != core->config.group_id) {
		return;
	}
	if (core->config.master_session == sync.common.master_session &&
	    core->slave_ack_wire_len != 0u) {
		return;
	}
	core->config.master_session = sync.common.master_session;
	core->slave_active = false;
	core->slave_last_downlink_sequence = 0u;
	core->slave_credit_bytes = 0u;
	core->slave_ack_replace = true;
	slave_prepare_ack(core);
}

static void slave_handle_poll(struct rb_scheduler_core *core,
			      const struct rb_radio_event_view *event,
			      uint64_t now_us)
{
	struct rb_poll poll;

	if (event->pipe != core->config.node_id ||
	    rb_poll_decode(event->wire, event->wire_len, &poll) != 0) {
		core->invalid_node_rx_count++;
		return;
	}
	if (poll.common.master_session != core->config.master_session) {
		core->invalid_session_rx_count++;
		return;
	}
	if (core->slave_uplink_inflight &&
	    poll.known_slave_session == core->config.slave_session &&
	    poll.uplink_ack_sequence == core->slave_uplink_inflight_sequence) {
		queue_discard(&core->slave_uplink_head,
			      &core->slave_uplink_count,
			      core->slave_uplink_inflight_len);
		core->slave_uplink_inflight = false;
		core->slave_uplink_inflight_len = 0u;
	}
	core->slave_credit_bytes = poll.next_credit_bytes;
	core->slave_active = true;
	core->slave_last_poll_us = now_us;
	core->slave_ack_replace = false;
	slave_prepare_ack(core);
}

static void slave_handle_downlink(struct rb_scheduler_core *core,
				  const struct rb_radio_event_view *event)
{
	struct rb_downlink_frame downlink;
	uint32_t distance;

	if (event->pipe != 0u ||
	    rb_downlink_decode(event->wire, event->wire_len, &downlink) != 0) {
		return;
	}
	if (downlink.common.master_session != core->config.master_session) {
		core->invalid_session_rx_count++;
		return;
	}
	if (core->slave_last_downlink_sequence != 0u &&
	    (downlink.sequence == core->slave_last_downlink_sequence ||
	     sequence_before(downlink.sequence,
			     core->slave_last_downlink_sequence))) {
		core->downlink_duplicate_count++;
		return;
	}
	distance = core->slave_last_downlink_sequence == 0u ? downlink.sequence :
		downlink.sequence - core->slave_last_downlink_sequence;
	if (distance > 1u) {
		core->downlink_gap_count += distance - 1u;
	}
	(void)queue_append(core->slave_uart_queue, &core->slave_uart_head,
			   &core->slave_uart_count, downlink.payload,
			   downlink.payload_len, &core->queue_drop_bytes);
	core->slave_last_downlink_sequence = downlink.sequence;
}

static void master_handle_ack(struct rb_scheduler_core *core,
			      const struct rb_radio_event_view *event,
			      uint64_t now_us)
{
	struct rb_ack_uplink ack;
	struct rb_scheduler_peer_runtime *runtime;
	enum rb_fixed_peer_event peer_event;
	bool duplicate = false;

	if (!node_valid(event->pipe) ||
	    rb_ack_uplink_decode(event->wire, event->wire_len, &ack) != 0 ||
	    ack.common.source_node != event->pipe) {
		core->invalid_node_rx_count++;
		return;
	}
	if (ack.common.master_session != core->config.master_session) {
		core->invalid_session_rx_count++;
		return;
	}
	runtime = peer_runtime(core, event->pipe);
	peer_event = rb_fixed_peer_note_ack(&core->fixed_peers, event->pipe,
					    ack.slave_session, now_us);
	if (peer_event == RB_FIXED_PEER_INVALID) {
		core->invalid_session_rx_count++;
		return;
	}
	if (peer_event == RB_FIXED_PEER_ACTIVATED ||
	    peer_event == RB_FIXED_PEER_SESSION_CHANGED) {
		runtime->uplink_ack_sequence = 0u;
		runtime->poll_interval_us = 0u;
		runtime->next_poll_due_us = now_us;
	}
	if (ack.payload_len != 0u && runtime->uplink_ack_sequence != 0u &&
	    (ack.uplink_sequence == runtime->uplink_ack_sequence ||
	     sequence_before(ack.uplink_sequence,
			     runtime->uplink_ack_sequence))) {
		duplicate = true;
		core->duplicate_rx_count++;
	}
	if (ack.payload_len != 0u && !duplicate) {
		int ret = rb_record_queue_push(
			&core->master_record_queue[event->pipe - 1u],
			ack.payload, ack.payload_len);

		if (ret == 0) {
			runtime->uplink_ack_sequence = ack.uplink_sequence;
		} else {
			core->master_record_drop_count[event->pipe - 1u]++;
			core->queue_drop_bytes += ack.payload_len;
		}
	}
	if (ack.payload_len != 0u) {
		rb_scheduler_mark_peer_data(core, event->pipe, now_us);
	} else {
		rb_scheduler_mark_peer_idle(core, event->pipe, now_us);
	}
}

void rb_scheduler_on_radio_event(struct rb_scheduler_core *core,
				 const struct rb_radio_event_view *event,
				 uint64_t now_us)
{
	struct rb_common_header common;

	if (core == NULL || event == NULL) {
		return;
	}
	if (event->type == RB_EVENT_VIEW_TX_SUCCESS ||
	    event->type == RB_EVENT_VIEW_TX_FAILED) {
		if (core->config.master && event->type == RB_EVENT_VIEW_TX_FAILED &&
		    core->in_flight_type == RB_ACTION_SEND_POLL &&
		    node_valid(core->in_flight_node)) {
			peer_runtime(core, core->in_flight_node)->poll_failure_count++;
		}
		core->transaction_in_flight = false;
		core->in_flight_node = 0u;
		core->in_flight_type = RB_ACTION_NONE;
		return;
	}
	if (event->type != RB_EVENT_VIEW_RX_RECEIVED || event->wire == NULL ||
	    rb_common_decode(event->wire, event->wire_len, &common) != 0) {
		return;
	}
	if (core->config.master) {
		if (common.type == RB_FRAME_ACK_UPLINK) {
			master_handle_ack(core, event, now_us);
		}
		return;
	}
	switch (common.type) {
	case RB_FRAME_SYNC:
		slave_handle_sync(core, event);
		break;
	case RB_FRAME_POLL:
		slave_handle_poll(core, event, now_us);
		break;
	case RB_FRAME_DOWNLINK_DATA:
		slave_handle_downlink(core, event);
		break;
	default:
		break;
	}
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
	} else {
		runtime->poll_interval_us *= 2u;
	}
	if (runtime->poll_interval_us > idle_limit(core)) {
		runtime->poll_interval_us = idle_limit(core);
	}
	runtime->next_poll_due_us = now_us + runtime->poll_interval_us;
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
	runtime->next_poll_due_us = now_us;
}

void rb_scheduler_set_sync_deadline(struct rb_scheduler_core *core,
				    uint64_t tick)
{
	if (core != NULL) {
		core->next_sync_tick = tick;
		core->probe_preceded_overdue_sync = false;
	}
}

uint32_t rb_scheduler_peer_poll_interval(const struct rb_scheduler_core *core,
					 uint8_t node_id)
{
	const struct rb_scheduler_peer_runtime *runtime =
		peer_runtime_const(core, node_id);

	return runtime == NULL ? 0u : runtime->poll_interval_us;
}

int64_t rb_scheduler_peer_ack_age_us(const struct rb_scheduler_core *core,
				     uint8_t node_id, uint64_t now_us)
{
	const struct rb_fixed_peer *peer = core == NULL ? NULL :
		rb_fixed_peer_get(&core->fixed_peers, node_id);
	uint64_t age;

	if (peer == NULL || !peer->active) {
		return -1;
	}
	if (now_us <= peer->last_ack_us) {
		return 0;
	}
	age = now_us - peer->last_ack_us;
	return age > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)age;
}

uint32_t rb_scheduler_peer_poll_failure_count(
	const struct rb_scheduler_core *core, uint8_t node_id)
{
	const struct rb_scheduler_peer_runtime *runtime =
		peer_runtime_const(core, node_id);

	return runtime == NULL ? 0u : runtime->poll_failure_count;
}

uint8_t rb_scheduler_active_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : rb_fixed_peer_active_count(&core->fixed_peers);
}

uint8_t rb_scheduler_active_mask(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : rb_fixed_peer_active_mask(&core->fixed_peers);
}

uint64_t rb_scheduler_queue_drop_bytes(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->queue_drop_bytes;
}

uint64_t rb_scheduler_duplicate_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->duplicate_rx_count;
}

uint64_t rb_scheduler_downlink_gap_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->downlink_gap_count;
}

uint64_t rb_scheduler_downlink_duplicate_count(
	const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->downlink_duplicate_count;
}

uint64_t rb_scheduler_invalid_session_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->invalid_session_rx_count;
}

uint64_t rb_scheduler_invalid_node_count(const struct rb_scheduler_core *core)
{
	return core == NULL ? 0u : core->invalid_node_rx_count;
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
	core->transaction_in_flight = false;
	core->action_retry_not_before_us = 0u;
	core->next_downlink_sequence = 1u;
	core->next_sync_tick = core->config.sync_interval_us;
	core->slave_last_downlink_sequence = 0u;
	core->slave_active = false;
	if (core->config.master) {
		rb_fixed_peer_table_init(&core->fixed_peers,
					 core->config.lease_timeout_us);
		memset(core->peer, 0, sizeof(core->peer));
		core->next_probe_due_us = 0u;
	} else {
		core->slave_ack_replace = true;
		slave_prepare_ack(core);
	}
	return 0;
}

size_t rb_scheduler_slave_read_uart(struct rb_scheduler_core *core,
				    uint8_t *data, size_t max_len)
{
	size_t len;

	if (core == NULL || data == NULL || max_len == 0u) {
		return 0u;
	}
	len = queue_peek(core->slave_uart_queue, core->slave_uart_head,
			 core->slave_uart_count, data, max_len);
	queue_discard(&core->slave_uart_head, &core->slave_uart_count, len);
	return len;
}

int rb_scheduler_master_peek_record(struct rb_scheduler_core *core,
				    uint8_t *node_id, uint8_t *data,
				    size_t max_len, size_t *record_len)
{
	if (core == NULL || !core->config.master || node_id == NULL || data == NULL ||
	    max_len == 0u || record_len == NULL) {
		return -EINVAL;
	}
	if (core->master_record_peek_valid) {
		*node_id = core->master_record_peek_node;
		return rb_record_queue_peek(
			&core->master_record_queue[*node_id - 1u], data, max_len,
			record_len);
	}
	for (size_t i = 0u; i < RB_SCHEDULER_MAX_PEERS; i++) {
		uint8_t candidate = (uint8_t)(
			((core->master_record_cursor + i) % RB_SCHEDULER_MAX_PEERS) + 1u);
		int ret = rb_record_queue_peek(
			&core->master_record_queue[candidate - 1u], data, max_len,
			record_len);

		if (ret == -EAGAIN) {
			continue;
		}
		if (ret != 0) {
			return ret;
		}
		core->master_record_peek_node = candidate;
		core->master_record_peek_valid = true;
		*node_id = candidate;
		return 0;
	}
	return -EAGAIN;
}

int rb_scheduler_master_pop_record(struct rb_scheduler_core *core,
				   uint8_t node_id)
{
	int ret;

	if (core == NULL || !core->config.master || !node_valid(node_id) ||
	    !core->master_record_peek_valid ||
	    node_id != core->master_record_peek_node) {
		return -EINVAL;
	}
	ret = rb_record_queue_pop(&core->master_record_queue[node_id - 1u]);
	if (ret == 0) {
		core->master_record_cursor = node_id % RB_SCHEDULER_MAX_PEERS;
		core->master_record_peek_node = 0u;
		core->master_record_peek_valid = false;
	}
	return ret;
}

uint64_t rb_scheduler_master_record_drop_count(
	const struct rb_scheduler_core *core, uint8_t node_id)
{
	return core == NULL || !node_valid(node_id) ? 0u :
		core->master_record_drop_count[node_id - 1u];
}
