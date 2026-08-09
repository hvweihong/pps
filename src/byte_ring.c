#include "byte_ring.h"

#include <stddef.h>
#include <string.h>

void rb_byte_ring_init(struct rb_byte_ring *ring, uint8_t *storage,
		       size_t capacity)
{
	if (ring == NULL) {
		return;
	}

	ring->storage = storage;
	ring->capacity = storage == NULL ? 0 : capacity;
	ring->read_index = 0;
	ring->write_index = 0;
	ring->length = 0;
	ring->dropped_bytes = 0;
}

static void copy_into_ring(struct rb_byte_ring *ring, const uint8_t *data,
			   size_t len)
{
	size_t first = ring->capacity - ring->write_index;

	if (first > len) {
		first = len;
	}
	memcpy(&ring->storage[ring->write_index], data, first);
	if (len > first) {
		memcpy(ring->storage, data + first, len - first);
	}
	ring->write_index = (ring->write_index + len) % ring->capacity;
	ring->length += len;
}

size_t rb_byte_ring_write(struct rb_byte_ring *ring, const uint8_t *data,
			  size_t len)
{
	size_t original_len = len;
	size_t overflow;

	if (ring == NULL || data == NULL || len == 0 || ring->storage == NULL ||
	    ring->capacity == 0) {
		return 0;
	}

	if (len >= ring->capacity) {
		size_t skipped = len - ring->capacity;

		ring->dropped_bytes += ring->length + skipped;
		data += skipped;
		len = ring->capacity;
		ring->read_index = 0;
		ring->write_index = 0;
		ring->length = 0;
		copy_into_ring(ring, data, len);
		ring->read_index = ring->write_index;
		return original_len;
	}

	overflow = ring->length + len > ring->capacity ?
		   ring->length + len - ring->capacity : 0;
	if (overflow != 0) {
		ring->read_index = (ring->read_index + overflow) % ring->capacity;
		ring->length -= overflow;
		ring->dropped_bytes += overflow;
	}

	copy_into_ring(ring, data, len);
	return original_len;
}

size_t rb_byte_ring_peek(const struct rb_byte_ring *ring, uint8_t *data,
			 size_t max_len)
{
	size_t count;
	size_t first;

	if (ring == NULL || data == NULL || max_len == 0 || ring->storage == NULL ||
	    ring->capacity == 0) {
		return 0;
	}

	count = ring->length < max_len ? ring->length : max_len;
	first = ring->capacity - ring->read_index;
	if (first > count) {
		first = count;
	}
	memcpy(data, &ring->storage[ring->read_index], first);
	if (count > first) {
		memcpy(data + first, ring->storage, count - first);
	}

	return count;
}

size_t rb_byte_ring_read(struct rb_byte_ring *ring, uint8_t *data,
			 size_t max_len)
{
	size_t count = rb_byte_ring_peek(ring, data, max_len);

	if (count != 0) {
		ring->read_index = (ring->read_index + count) % ring->capacity;
		ring->length -= count;
	}
	return count;
}

size_t rb_byte_ring_discard(struct rb_byte_ring *ring, size_t max_len)
{
	size_t count;

	if (ring == NULL || ring->capacity == 0u || max_len == 0u) {
		return 0u;
	}
	count = ring->length < max_len ? ring->length : max_len;
	ring->read_index = (ring->read_index + count) % ring->capacity;
	ring->length -= count;
	return count;
}

size_t rb_byte_ring_size(const struct rb_byte_ring *ring)
{
	return ring == NULL ? 0 : ring->length;
}

void rb_byte_ring_clear(struct rb_byte_ring *ring)
{
	if (ring == NULL) {
		return;
	}

	ring->read_index = 0;
	ring->write_index = 0;
	ring->length = 0;
	ring->dropped_bytes = 0;
}
