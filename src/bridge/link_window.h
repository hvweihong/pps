#ifndef LINK_WINDOW_H_
#define LINK_WINDOW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_protocol.h"

#define RB_LINK_WINDOW_SIZE 64u
#define RB_PACKET_DATA_MAX (RB_ESB_MAX_PAYLOAD - RB_DATA_HEADER_SIZE)

static inline bool rb_seq_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

struct rb_packet_slot {
	bool occupied;
	uint32_t sequence;
	size_t data_len;
	uint8_t data[RB_PACKET_DATA_MAX];
};

struct rb_tx_window {
	uint16_t stream_epoch;
	struct rb_packet_slot *slots;
	size_t slot_count;
	size_t count;
	bool eviction_pending;
	uint32_t evicted_sequence;
};

struct rb_rx_window {
	uint16_t stream_epoch;
	uint32_t ack_base;
	uint64_t ack_bitmap;
	uint32_t next_release_sequence;
	struct rb_packet_slot *slots;
	size_t slot_count;
	size_t count;
};

void rb_tx_window_init(struct rb_tx_window *window, uint16_t stream_epoch,
		       struct rb_packet_slot *slots, size_t slot_count);
void rb_tx_window_reset_epoch(struct rb_tx_window *window, uint16_t stream_epoch);
int rb_tx_window_store(struct rb_tx_window *window, uint32_t sequence,
		       const uint8_t *data, size_t data_len);
void rb_tx_window_apply_ack(struct rb_tx_window *window, uint32_t ack_base,
			    uint64_t ack_bitmap);
const struct rb_packet_slot *rb_tx_window_next_repair(
	const struct rb_tx_window *window);
bool rb_tx_window_contains(const struct rb_tx_window *window, uint32_t sequence);
bool rb_tx_window_take_evicted(struct rb_tx_window *window,
			       uint32_t *sequence);

void rb_rx_window_init(struct rb_rx_window *window, uint16_t stream_epoch,
		       uint32_t first_sequence, struct rb_packet_slot *slots,
		       size_t slot_count);
void rb_rx_window_reset_epoch(struct rb_rx_window *window, uint16_t stream_epoch,
			      uint32_t first_sequence);
int rb_rx_window_insert(struct rb_rx_window *window, uint32_t sequence,
			const uint8_t *data, size_t data_len);
int rb_rx_window_pop(struct rb_rx_window *window, uint32_t *sequence,
		     uint8_t *data, size_t data_size, size_t *data_len);
void rb_rx_window_skip_to(struct rb_rx_window *window, uint32_t next_sequence);
bool rb_rx_window_contains(const struct rb_rx_window *window, uint32_t sequence);

#endif /* LINK_WINDOW_H_ */
