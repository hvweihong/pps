#ifndef RADIO_TRANSPORT_H_
#define RADIO_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "link_protocol.h"
#include "radio_address.h"

#define RB_RADIO_RETAINED_HISTORY_SIZE 16u

enum rb_radio_boot_stage {
	RB_RADIO_BOOT_STAGE_NONE,
	RB_RADIO_BOOT_STAGE_TRANSPORT_INIT = 0xd1,
	RB_RADIO_BOOT_STAGE_PROFILE_SWITCH,
	RB_RADIO_BOOT_STAGE_STOP_RX,
	RB_RADIO_BOOT_STAGE_STOP_RX_DONE,
	RB_RADIO_BOOT_STAGE_DISABLE,
	RB_RADIO_BOOT_STAGE_DISABLE_DONE,
	RB_RADIO_BOOT_STAGE_ESB_INIT,
	RB_RADIO_BOOT_STAGE_ESB_INIT_DONE,
	RB_RADIO_BOOT_STAGE_ADDRESS_CONFIG,
	RB_RADIO_BOOT_STAGE_ADDRESS_CONFIG_DONE,
	RB_RADIO_BOOT_STAGE_ESB_START_RX,
	RB_RADIO_BOOT_STAGE_ESB_START_RX_DONE,
	RB_RADIO_BOOT_STAGE_PROFILE_RETURNED,
	RB_RADIO_BOOT_STAGE_SCHEDULER_NEXT,
	RB_RADIO_BOOT_STAGE_SCHEDULER_NEXT_RETURNED,
	RB_RADIO_BOOT_STAGE_ACTION,
	RB_RADIO_BOOT_STAGE_ACTION_RETURNED,
	RB_RADIO_BOOT_STAGE_SEND,
	RB_RADIO_BOOT_STAGE_SEND_RETURNED,
	RB_RADIO_BOOT_STAGE_EVENT_HANDLER,
	RB_RADIO_BOOT_STAGE_EVENT_CAPTURED,
	RB_RADIO_BOOT_STAGE_EVENT_RETURNED,
};

struct rb_radio_retained_diag_entry {
	uint32_t sequence;
	uint32_t cycle;
	int32_t result;
	uint16_t payload_length;
	uint8_t stage;
	uint8_t runtime_action;
	uint8_t radio_event_id;
	uint8_t radio_state;
	uint8_t current_profile;
	uint8_t target_profile;
	uint8_t esb_active;
	uint8_t primask;
};

struct rb_radio_retained_diag {
	uint32_t magic;
	uint32_t magic_inverse;
	uint32_t update_count;
	uint32_t switch_index;
	uint8_t stage;
	uint8_t current_profile;
	uint8_t target_profile;
	uint8_t esb_active;
	uint8_t runtime_action;
	uint8_t radio_event_id;
	uint16_t payload_length;
	int32_t last_result;
	uint32_t radio_state;
	uint32_t events_disabled;
	uint32_t hfclkstat;
	uint32_t primask;
	uint32_t ipsr;
	uint8_t history_count;
	uint8_t history_next;
	struct rb_radio_retained_diag_entry history[RB_RADIO_RETAINED_HISTORY_SIZE];
};

enum rb_radio_profile {
	RB_RADIO_MASTER_PTX,
	RB_RADIO_SLAVE_PRX,
};

enum rb_radio_event_type {
	RB_RADIO_EVENT_RX_RECEIVED,
	RB_RADIO_EVENT_TX_SUCCESS,
	RB_RADIO_EVENT_TX_FAILED,
};

struct rb_radio_transport_config {
	bool master;
	uint8_t node_id;
	uint32_t group_id;
	struct rb_radio_addresses addresses;
};

struct rb_radio_event {
	enum rb_radio_event_type type;
	uint8_t pipe;
	uint8_t attempts;
	uint16_t length;
	uint64_t address_tick;
	uint8_t data[RB_ESB_MAX_PAYLOAD];
};

int radio_transport_init(const struct rb_radio_transport_config *config);
int radio_transport_send(uint8_t pipe, bool no_ack,
				const uint8_t *data, size_t len);
int radio_transport_queue_ack(uint8_t pipe, const uint8_t *data, size_t len);
int radio_transport_replace_ack(uint8_t pipe, const uint8_t *data, size_t len);
int radio_transport_get_event(struct rb_radio_event *event, k_timeout_t timeout);
uint32_t radio_transport_event_drop_count(void);
bool radio_transport_retained_diag_get(struct rb_radio_retained_diag *diag);
void radio_transport_retained_diag_clear(void);
void radio_transport_retained_diag_note(enum rb_radio_boot_stage stage,
					uint8_t runtime_action, int result);

typedef void (*rb_radio_transport_wake_fn)(void);
/* cb is invoked from ESB event-handler (ISR) context whenever an event is
 * queued, so the bridge thread can react before its fallback poll timeout
 * elapses.  cb itself must be ISR-safe; it must not log, block, or allocate. */
void radio_transport_set_wake_callback(rb_radio_transport_wake_fn cb);

#ifdef CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION
/* Inject packet loss for testing.  type_mask is a bitmask of rb_frame_type
 * values to drop; every_n drops one in every N frames of matching type.
 * Pass 0,0 to disable.  Never call from production code paths. */
void radio_transport_loss_set(uint32_t type_mask, uint32_t every_n);
void radio_transport_loss_once(uint32_t type_mask, size_t minimum_length);
uint32_t radio_transport_loss_drop_count(void);
#endif

#endif /* RADIO_TRANSPORT_H_ */
