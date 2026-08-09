#include "link_window.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static bool window_storage_valid(const struct rb_packet_slot *slots,
				 size_t slot_count)
{
	return slots != NULL && slot_count > 0 &&
		slot_count <= RB_LINK_WINDOW_SIZE;
}

static void clear_slots(struct rb_packet_slot *slots, size_t slot_count)
{
	for (size_t i = 0; i < slot_count; i++) {
		slots[i].occupied = false;
		slots[i].data_len = 0;
	}
}

static struct rb_packet_slot *find_slot(struct rb_packet_slot *slots,
					size_t slot_count, uint32_t sequence)
{
	for (size_t i = 0; i < slot_count; i++) {
		if (slots[i].occupied && slots[i].sequence == sequence) {
			return &slots[i];
		}
	}
	return NULL;
}

static const struct rb_packet_slot *find_slot_const(
	const struct rb_packet_slot *slots, size_t slot_count, uint32_t sequence)
{
	for (size_t i = 0; i < slot_count; i++) {
		if (slots[i].occupied && slots[i].sequence == sequence) {
			return &slots[i];
		}
	}
	return NULL;
}

static struct rb_packet_slot *find_free_slot(struct rb_packet_slot *slots,
					     size_t slot_count)
{
	for (size_t i = 0; i < slot_count; i++) {
		if (!slots[i].occupied) {
			return &slots[i];
		}
	}
	return NULL;
}

void rb_tx_window_init(struct rb_tx_window *window, uint16_t stream_epoch,
		       struct rb_packet_slot *slots, size_t slot_count)
{
	if (window == NULL) {
		return;
	}
	window->stream_epoch = stream_epoch;
	window->slots = slots;
	window->slot_count = window_storage_valid(slots, slot_count) ? slot_count : 0;
	window->count = 0;
	window->eviction_pending = false;
	window->evicted_sequence = 0;
	if (window->slot_count != 0) {
		clear_slots(slots, slot_count);
	}
}

void rb_tx_window_reset_epoch(struct rb_tx_window *window, uint16_t stream_epoch)
{
	if (window == NULL) {
		return;
	}
	window->stream_epoch = stream_epoch;
	window->count = 0;
	window->eviction_pending = false;
	if (window->slot_count != 0) {
		clear_slots(window->slots, window->slot_count);
	}
}

static struct rb_packet_slot *oldest_slot(struct rb_packet_slot *slots,
					  size_t slot_count)
{
	struct rb_packet_slot *oldest = NULL;

	for (size_t i = 0; i < slot_count; i++) {
		if (!slots[i].occupied) {
			continue;
		}
		if (oldest == NULL || rb_seq_before(slots[i].sequence,
						     oldest->sequence)) {
			oldest = &slots[i];
		}
	}
	return oldest;
}

int rb_tx_window_store(struct rb_tx_window *window, uint32_t sequence,
		       const uint8_t *data, size_t data_len)
{
	struct rb_packet_slot *slot;

	if (window == NULL || window->stream_epoch == 0 || window->slot_count == 0 ||
	    (data == NULL && data_len != 0)) {
		return -EINVAL;
	}
	if (data_len > RB_PACKET_DATA_MAX) {
		return -EMSGSIZE;
	}
	if (find_slot(window->slots, window->slot_count, sequence) != NULL) {
		return -EALREADY;
	}

	slot = find_free_slot(window->slots, window->slot_count);
	if (slot == NULL) {
		slot = oldest_slot(window->slots, window->slot_count);
		window->evicted_sequence = slot->sequence;
		window->eviction_pending = true;
		window->count--;
	}

	slot->occupied = true;
	slot->sequence = sequence;
	slot->data_len = data_len;
	if (data_len != 0) {
		memcpy(slot->data, data, data_len);
	}
	window->count++;
	return 0;
}

void rb_tx_window_apply_ack(struct rb_tx_window *window, uint32_t ack_base,
			    uint64_t ack_bitmap)
{
	if (window == NULL || window->slot_count == 0) {
		return;
	}

	for (size_t i = 0; i < window->slot_count; i++) {
		struct rb_packet_slot *slot = &window->slots[i];
		uint32_t distance;
		bool acknowledged;

		if (!slot->occupied) {
			continue;
		}
		acknowledged = slot->sequence == ack_base ||
			rb_seq_before(slot->sequence, ack_base);
		distance = slot->sequence - ack_base;
		if (!acknowledged && distance >= 1 && distance <= 64) {
			acknowledged = (ack_bitmap & (UINT64_C(1) << (distance - 1))) != 0;
		}
		if (acknowledged) {
			slot->occupied = false;
			slot->data_len = 0;
			window->count--;
		}
	}
}

const struct rb_packet_slot *rb_tx_window_next_repair(
	const struct rb_tx_window *window)
{
	if (window == NULL || window->slot_count == 0) {
		return NULL;
	}
	return oldest_slot(window->slots, window->slot_count);
}

bool rb_tx_window_contains(const struct rb_tx_window *window, uint32_t sequence)
{
	return window != NULL && window->slot_count != 0 &&
		find_slot_const(window->slots, window->slot_count, sequence) != NULL;
}

bool rb_tx_window_take_evicted(struct rb_tx_window *window, uint32_t *sequence)
{
	if (window == NULL || sequence == NULL || !window->eviction_pending) {
		return false;
	}
	*sequence = window->evicted_sequence;
	window->eviction_pending = false;
	return true;
}

void rb_rx_window_init(struct rb_rx_window *window, uint16_t stream_epoch,
		       uint32_t first_sequence, struct rb_packet_slot *slots,
		       size_t slot_count)
{
	if (window == NULL) {
		return;
	}
	window->slots = slots;
	window->slot_count = window_storage_valid(slots, slot_count) ? slot_count : 0;
	rb_rx_window_reset_epoch(window, stream_epoch, first_sequence);
}

void rb_rx_window_reset_epoch(struct rb_rx_window *window, uint16_t stream_epoch,
			      uint32_t first_sequence)
{
	if (window == NULL) {
		return;
	}
	window->stream_epoch = stream_epoch;
	window->ack_base = first_sequence - 1;
	window->ack_bitmap = 0;
	window->next_release_sequence = first_sequence;
	window->count = 0;
	if (window->slot_count != 0) {
		clear_slots(window->slots, window->slot_count);
	}
}

static void advance_ack_base(struct rb_rx_window *window)
{
	while ((window->ack_bitmap & UINT64_C(1)) != 0) {
		window->ack_base++;
		window->ack_bitmap >>= 1;
	}
}

int rb_rx_window_insert(struct rb_rx_window *window, uint32_t sequence,
			const uint8_t *data, size_t data_len)
{
	struct rb_packet_slot *slot;
	uint32_t distance;

	if (window == NULL || window->stream_epoch == 0 || window->slot_count == 0 ||
	    (data == NULL && data_len != 0)) {
		return -EINVAL;
	}
	if (data_len > RB_PACKET_DATA_MAX) {
		return -EMSGSIZE;
	}
	if (sequence == window->ack_base || rb_seq_before(sequence, window->ack_base) ||
	    find_slot(window->slots, window->slot_count, sequence) != NULL) {
		return -EALREADY;
	}
	distance = sequence - window->ack_base;
	if (distance == 0 || distance > RB_LINK_WINDOW_SIZE) {
		return -ERANGE;
	}
	slot = find_free_slot(window->slots, window->slot_count);
	if (slot == NULL) {
		return -ENOSPC;
	}

	slot->occupied = true;
	slot->sequence = sequence;
	slot->data_len = data_len;
	if (data_len != 0) {
		memcpy(slot->data, data, data_len);
	}
	window->count++;
	window->ack_bitmap |= UINT64_C(1) << (distance - 1);
	advance_ack_base(window);
	return 0;
}

int rb_rx_window_pop(struct rb_rx_window *window, uint32_t *sequence,
		     uint8_t *data, size_t data_size, size_t *data_len)
{
	struct rb_packet_slot *slot;

	if (window == NULL || sequence == NULL || data_len == NULL ||
	    (data == NULL && data_size != 0)) {
		return -EINVAL;
	}
	slot = find_slot(window->slots, window->slot_count,
			 window->next_release_sequence);
	if (slot == NULL) {
		return -EAGAIN;
	}
	if (data_size < slot->data_len) {
		return -EMSGSIZE;
	}
	if (slot->data_len != 0) {
		memcpy(data, slot->data, slot->data_len);
	}
	*sequence = slot->sequence;
	*data_len = slot->data_len;
	slot->occupied = false;
	slot->data_len = 0;
	window->count--;
	window->next_release_sequence++;
	return 0;
}

void rb_rx_window_skip_to(struct rb_rx_window *window, uint32_t next_sequence)
{
	if (window == NULL || window->slot_count == 0 ||
	    rb_seq_before(next_sequence, window->next_release_sequence)) {
		return;
	}

	window->next_release_sequence = next_sequence;
	window->ack_base = next_sequence - 1;
	window->ack_bitmap = 0;
	for (size_t i = 0; i < window->slot_count; i++) {
		struct rb_packet_slot *slot = &window->slots[i];
		uint32_t distance;

		if (!slot->occupied) {
			continue;
		}
		if (rb_seq_before(slot->sequence, next_sequence)) {
			slot->occupied = false;
			slot->data_len = 0;
			window->count--;
			continue;
		}
		distance = slot->sequence - window->ack_base;
		if (distance == 0 || distance > RB_LINK_WINDOW_SIZE) {
			slot->occupied = false;
			slot->data_len = 0;
			window->count--;
			continue;
		}
		window->ack_bitmap |= UINT64_C(1) << (distance - 1);
	}
	advance_ack_base(window);
}

bool rb_rx_window_contains(const struct rb_rx_window *window, uint32_t sequence)
{
	return window != NULL && window->slot_count != 0 &&
		find_slot_const(window->slots, window->slot_count, sequence) != NULL;
}
