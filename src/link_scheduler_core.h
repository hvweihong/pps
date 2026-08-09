#ifndef LINK_SCHEDULER_CORE_H_
#define LINK_SCHEDULER_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_protocol.h"
#include "link_window.h"
#include "membership.h"

enum rb_scheduler_action_type {
	RB_ACTION_NONE,
	RB_ACTION_SEND_SYNC_DISCOVERY,
	RB_ACTION_ENTER_DISCOVERY_RX,
	RB_ACTION_SEND_ASSIGN,
	RB_ACTION_SEND_DOWNLINK_BROADCAST,
	RB_ACTION_SEND_REPAIR,
	RB_ACTION_SEND_SKIP_TO,
	RB_ACTION_SEND_POLL,
	RB_ACTION_QUEUE_ACK,
	RB_ACTION_SEND_HELLO,
	RB_ACTION_ENTER_ASSIGN_RX,
	RB_ACTION_ENTER_GROUP_RX,
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
	uint64_t target_device_id;
	bool no_ack;
	uint64_t due_tick;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t wire_len;
};

struct rb_scheduler_config {
	bool master;
	uint32_t group_id;
	uint32_t master_session;
	uint64_t device_id;
	uint32_t sync_interval_us;
	uint32_t aggregation_timeout_us;
	uint32_t max_idle_poll_us;
	uint32_t lease_timeout_us;
	uint32_t assignment_window_us;
	uint8_t response_slot_count;
	uint16_t response_slot_us;
};

#define RB_SCHEDULER_MAX_PEERS RB_MEMBERSHIP_MAX_PEERS
#define RB_SCHEDULER_UART_QUEUE_SIZE 16384u
#define RB_SCHEDULER_DEFAULT_DOWNLINK_EPOCH 1u
#define RB_SCHEDULER_DEFAULT_UPLINK_EPOCH 1u
#define RB_SCHEDULER_DEFAULT_IDLE_POLL_US 1000u

struct rb_scheduler_peer_runtime {
	uint32_t next_poll_due_us;
	uint32_t poll_interval_us;
	uint32_t poll_sequence;
	uint32_t uplink_ack_base;
	uint32_t missing_sequence;
	bool data_ready;
	bool repair_pending;
	bool skip_pending;
	bool poll_retry_backoff;
};

struct rb_scheduler_candidate {
	uint64_t device_id;
	uint32_t capabilities;
	uint32_t discovery_nonce;
	bool queued;
};

struct rb_scheduler_core {
	struct rb_scheduler_config config;
	struct rb_membership membership;
	struct rb_scheduler_peer_runtime peer[RB_SCHEDULER_MAX_PEERS];
	struct rb_scheduler_candidate candidates[RB_SCHEDULER_MAX_PEERS];
	uint8_t candidate_count;
	uint8_t candidate_next;
	uint8_t poll_cursor;
	uint8_t free_slots;
	uint32_t downlink_epoch;
	uint32_t uplink_epoch;
	uint32_t next_downlink_sequence;
	uint64_t next_sync_tick;
	uint64_t first_uart_tick;
	uint8_t uart_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t uart_head;
	uint16_t uart_count;
	uint8_t queued_wire[RB_ESB_MAX_PAYLOAD];
	size_t queued_wire_len;
	bool queued_broadcast;
	bool discovery_rx_pending;
	uint64_t response_window_deadline;
	bool sync_in_flight;
	bool transaction_in_flight;
	/* Failed action submissions must not be retried in a tight loop. */
	uint64_t action_retry_not_before_us;
	uint8_t in_flight_node;
	uint8_t in_flight_pipe;
	enum rb_scheduler_action_type in_flight_type;
	bool in_flight_poll_data_ready;
	uint16_t next_lease_id;
	uint16_t next_stream_epoch;
	struct rb_packet_slot downlink_slots[RB_LINK_WINDOW_SIZE];
	struct rb_tx_window downlink_history;
	/* Candidate/assigned-slave state is kept here so the same pure core can
	 * serve Task 13 without a second scheduler implementation. */
	bool slave_active;
	uint8_t slave_node_id;
	uint16_t slave_lease_id;
	uint16_t slave_downlink_epoch;
	uint16_t slave_uplink_epoch;
	uint64_t slave_hello_due_tick;
	uint32_t slave_discovery_nonce;
	uint8_t slave_discovery_slot;
	bool slave_discovery_slot_valid;
	uint64_t slave_assign_rx_deadline;
	bool slave_hello_pending;
	bool slave_assign_rx_pending;
	bool slave_assign_rx_active;
	bool slave_group_rx_pending;
	bool slave_ack_pending;
	bool slave_ack_ready;
	uint8_t slave_ack_wire[RB_ESB_MAX_PAYLOAD];
	size_t slave_ack_wire_len;
	uint16_t slave_credit_bytes;
	uint16_t slave_last_poll_sequence;
	uint32_t slave_uplink_inflight_sequence;
	size_t slave_uplink_inflight_len;
	bool slave_uplink_inflight;
	struct rb_packet_slot slave_rx_slots[RB_LINK_WINDOW_SIZE];
	struct rb_rx_window slave_rx_window;
	struct rb_slave_lease slave_lease;
	uint8_t slave_uart_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t slave_uart_head;
	uint16_t slave_uart_count;
	uint32_t slave_uplink_sequence;
	uint8_t slave_uplink_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t slave_uplink_head;
	uint16_t slave_uplink_count;
	uint8_t master_rx_queue[RB_SCHEDULER_UART_QUEUE_SIZE];
	uint16_t master_rx_head;
	uint16_t master_rx_count;
	uint64_t queue_drop_bytes;
	uint64_t duplicate_rx_count;
	uint64_t invalid_session_rx_count;
};

void rb_scheduler_init(struct rb_scheduler_core *core,
			 const struct rb_scheduler_config *config);
int rb_scheduler_add_active_peer(struct rb_scheduler_core *core,
				 uint8_t node_id, uint64_t device_id,
				 uint16_t lease_id, uint64_t now_us);
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
void rb_scheduler_note_downlink_ack(struct rb_scheduler_core *core,
					uint8_t node_id, uint32_t ack_base,
					uint64_t ack_bitmap);
void rb_scheduler_mark_peer_idle(struct rb_scheduler_core *core,
					 uint8_t node_id, uint64_t now_us);
void rb_scheduler_mark_peer_data(struct rb_scheduler_core *core,
					 uint8_t node_id, uint64_t now_us);
void rb_scheduler_mark_peer_suspect(struct rb_scheduler_core *core,
					    uint8_t node_id, uint64_t now_us);
void rb_scheduler_set_sync_deadline(struct rb_scheduler_core *core,
					    uint64_t tick);
void rb_scheduler_set_free_slots(struct rb_scheduler_core *core,
				 uint8_t free_slots);
void rb_scheduler_set_discovery_slot(struct rb_scheduler_core *core,
					 uint8_t slot);
uint32_t rb_scheduler_peer_poll_interval(const struct rb_scheduler_core *core,
						 uint8_t node_id);
uint8_t rb_scheduler_active_count(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_queue_drop_bytes(const struct rb_scheduler_core *core);
bool rb_scheduler_take_evicted(struct rb_scheduler_core *core, uint32_t *sequence);
uint64_t rb_scheduler_duplicate_count(const struct rb_scheduler_core *core);
uint64_t rb_scheduler_invalid_session_count(const struct rb_scheduler_core *core);
uint32_t rb_scheduler_session(const struct rb_scheduler_core *core);
int rb_scheduler_reset_session(struct rb_scheduler_core *core,
			       uint32_t master_session);
size_t rb_scheduler_slave_read_uart(struct rb_scheduler_core *core,
					uint8_t *data, size_t max_len);
size_t rb_scheduler_master_read_uart(struct rb_scheduler_core *core,
					 uint8_t *data, size_t max_len);

#endif /* LINK_SCHEDULER_CORE_H_ */
