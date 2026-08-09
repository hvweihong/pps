#ifndef BYTE_RING_H_
#define BYTE_RING_H_

#include <stddef.h>
#include <stdint.h>

struct rb_byte_ring {
	uint8_t *storage;
	size_t capacity;
	size_t read_index;
	size_t write_index;
	size_t length;
	uint64_t dropped_bytes;
};

void rb_byte_ring_init(struct rb_byte_ring *ring, uint8_t *storage,
		       size_t capacity);
size_t rb_byte_ring_write(struct rb_byte_ring *ring, const uint8_t *data,
			  size_t len);
size_t rb_byte_ring_read(struct rb_byte_ring *ring, uint8_t *data,
			 size_t max_len);
size_t rb_byte_ring_discard(struct rb_byte_ring *ring, size_t max_len);
size_t rb_byte_ring_peek(const struct rb_byte_ring *ring, uint8_t *data,
			 size_t max_len);
size_t rb_byte_ring_size(const struct rb_byte_ring *ring);
void rb_byte_ring_clear(struct rb_byte_ring *ring);

#endif /* BYTE_RING_H_ */
