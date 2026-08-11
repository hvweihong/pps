#ifndef LINK_SCHEDULER_CORE_H_
#define LINK_SCHEDULER_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fixed_peer_table.h"
#include "link_protocol.h"
#include "record_queue.h"

enum rb_scheduler_action_type {
	RB_ACTION_NONE,
	RB_ACTION_SEND_SYNC,
	RB_ACTION_SEND_DOWNLINK_BROADCAST,
	RB_ACTION_SEND_POLL,
	RB_ACTION_QUEUE_ACK,
};

enum rb_radio_event_view_type {
	RB_EVENT_VIEW_RX_RECEIVED,
	RB_EVENT_VIEW_TX_SUCCESS,
	RB_EVENT_VIEW_TX_FAILED,
};

struct rb_radio_event_view {
	enum rb_radio_event_view_type type;
	uint8_t pipe;
	uint8_t attempts;
	uint64_t address_tick;
	const uint8_t *wire;
	size_t wire_len;
};

struct rb_scheduler_action {
	enum rb_scheduler_action_type type;
	uint8_t node_id;
	uint8_t pipe;
	bool no_ack;
	bool replace_ack;
	uint64_t due_tick;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;
};

struct rb_scheduler_config {
	bool master;
	uint8_t node_id;
	uint32_t group_id;
	uint32_t master_session;
	uint32_t slave_session;
	uint64_t local_device_id;
	uint32_t sync_interval_us;
	uint32_t aggregation_timeout_us;
	uint32_t max_idle_poll_us;
	uint32_t lease_timeout_us;
};

struct rb_scheduler_time_publication {
	int64_t next_pps_utc_seconds;
	uint8_t time_quality;
};

#define RB_SCHEDULER_MAX_PEERS RB_FIXED_PEER_COUNT
#define RB_SCHEDULER_UART_QUEUE_SIZE 16384u
#define RB_SCHEDULER_DEFAULT_IDLE_POLL_US 1000u
#define RB_SCHEDULER_INACTIVE_PROBE_US 20000u
#define RB_SCHEDULER_RECORD_LENGTH_CAPACITY \
	((RB_SCHEDULER_UART_QUEUE_SIZE + RB_ACK_UPLINK_PAYLOAD_MAX - 1u) / \
	 RB_ACK_UPLINK_PAYLOAD_MAX)

struct rb_scheduler_peer_runtime {
	uint64_t next_poll_due_us;
	uint32_t poll_interval_us;
	uint16_t poll_sequence;
	uint32_t uplink_ack_sequence;
	uint32_t poll_failure_count;
	bool data_ready;
};

struct rb_scheduler_core {
	struct rb_scheduler_config config;
	struct rb_fixed_peer_table fixed_peers;
	struct rb_scheduler_peer_runtime peer[RB_SCHEDULER_MAX_PEERS];
	uint8_t poll_cursor;
	uint8_t probe_cursor;
	uint64_t next_probe_due_us;
	bool probe_preceded_overdue_sync;
	uint32_t next_downlink_sequence;
	uint64_t next_sync_tick;
	uint64_t first_uart_tick;
	uint8_t uart_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t uart_head;
	uint16_t uart_count;
	uint8_t queued_wire[RB_ESB_MAX_PAYLOAD];
	size_t queued_wire_len;
	bool queued_broadcast;
	bool transaction_in_flight;
	uint64_t action_retry_not_before_us;
	uint8_t in_flight_node;
	enum rb_scheduler_action_type in_flight_type;
	bool slave_active;
	uint8_t slave_node_id;
	uint64_t slave_last_poll_us;
	bool slave_ack_pending;
	bool slave_ack_replace;
	uint8_t slave_ack_wire[RB_ESB_MAX_PAYLOAD];
	size_t slave_ack_wire_len;
	uint16_t slave_credit_bytes;
	uint32_t slave_uplink_inflight_sequence;
	size_t slave_uplink_inflight_len;
	bool slave_uplink_inflight;
	uint32_t slave_last_downlink_sequence;
	uint8_t slave_uart_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t slave_uart_head;
	uint16_t slave_uart_count;
	uint32_t slave_uplink_sequence;
	uint8_t slave_uplink_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t slave_uplink_head;
	uint16_t slave_uplink_count;
	struct rb_record_queue master_record_queue[RB_SCHEDULER_MAX_PEERS];
	uint8_t master_record_storage[RB_SCHEDULER_MAX_PEERS]
		[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t master_record_lengths[RB_SCHEDULER_MAX_PEERS]
		[RB_SCHEDULER_RECORD_LENGTH_CAPACITY];
	uint64_t master_record_drop_count[RB_SCHEDULER_MAX_PEERS];
	uint8_t master_record_cursor;
	uint8_t master_record_peek_node;
	bool master_record_peek_valid;
	uint64_t queue_drop_bytes;
	uint64_t duplicate_rx_count;
	uint64_t downlink_gap_count;
	uint64_t downlink_duplicate_count;
	uint64_t invalid_session_rx_count;
	uint64_t invalid_node_rx_count;
	struct rb_scheduler_time_publication time_publication;
};

void rb_scheduler_init(struct rb_scheduler_core *core,
		       const struct rb_scheduler_config *config);
int rb_scheduler_queue_downlink(struct rb_scheduler_core *core,
			const uint8_t *data, size_t len);
size_t rb_scheduler_uart_write(struct rb_scheduler_core *core,
			       const uint8_t *data, size_t len, uint64_t now_us);
size_t rb_scheduler_uart_available(const struct rb_scheduler_core *core);
int rb_scheduler_next_action(struct rb_scheduler_core *core, uint64_t now_us,
			     struct rb_scheduler_action *action);
void rb_scheduler_action_failed(struct rb_scheduler_core *core,
				const struct rb_scheduler_action *action,
				uint64_t now_us);
void rb_scheduler_on_radio_event(struct rb_scheduler_core *core,
				 const struct rb_radio_event_view *event,
				 uint64_t now_us);
void rb_scheduler_mark_peer_idle(struct rb_scheduler_core *core,
				 uint8_t node_id, uint64_t now_us);
void rb_scheduler_mark_peer_data(struct rb_scheduler_core *core,
				 uint8_t node_id, uint64_t now_us);
void rb_scheduler_set_sync_deadline(struct rb_scheduler_core *core,
				    uint64_t tick);
int rb_scheduler_set_time_publication(struct rb_scheduler_core *core,
				      int64_t next_pps_utc_seconds,
				      uint8_t time_quality);
uint32_t rb_scheduler_peer_poll_interval(const struct rb_scheduler_core *core,
					 uint8_t node_id);
int64_t rb_scheduler_peer_ack_age_us(const struct rb_scheduler_core *core,
				     uint8_t node_id, uint64_t now_us);
uint32_t rb_scheduler_peer_poll_failure_count(
	const struct rb_scheduler_core *core, uint8_t node_id);
uint8_t rb_scheduler_active_count(const struct rb_scheduler_core *core);
uint8_t rb_scheduler_active_mask(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_queue_drop_bytes(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_duplicate_count(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_downlink_gap_count(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_downlink_duplicate_count(
	const struct rb_scheduler_core *core);
uint64_t rb_scheduler_invalid_session_count(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_invalid_node_count(const struct rb_scheduler_core *core);
uint32_t rb_scheduler_session(const struct rb_scheduler_core *core);
int rb_scheduler_reset_session(struct rb_scheduler_core *core,
			       uint32_t master_session);
size_t rb_scheduler_slave_read_uart(struct rb_scheduler_core *core,
				    uint8_t *data, size_t max_len);
int rb_scheduler_master_peek_record(struct rb_scheduler_core *core,
				    uint8_t *node_id, uint8_t *data,
				    size_t max_len, size_t *record_len);
int rb_scheduler_master_pop_record(struct rb_scheduler_core *core,
				   uint8_t node_id);
uint64_t rb_scheduler_master_record_drop_count(
	const struct rb_scheduler_core *core, uint8_t node_id);

#endif /* LINK_SCHEDULER_CORE_H_ */
