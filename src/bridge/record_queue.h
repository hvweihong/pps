#ifndef RECORD_QUEUE_H_
#define RECORD_QUEUE_H_

#include <stddef.h>
#include <stdint.h>

struct rb_record_queue {
	uint8_t *storage;
	size_t storage_size;
	size_t data_head;
	size_t data_tail;
	size_t data_count;
	uint16_t *lengths;
	size_t length_capacity;
	size_t length_head;
	size_t length_tail;
	size_t length_count;
};

void rb_record_queue_init(struct rb_record_queue *queue,
			  uint8_t *storage, size_t storage_size,
			  uint16_t *lengths, size_t length_capacity);
int rb_record_queue_push(struct rb_record_queue *queue,
			 const uint8_t *data, size_t len);
int rb_record_queue_peek(const struct rb_record_queue *queue,
			 uint8_t *data, size_t max_len, size_t *record_len);
int rb_record_queue_pop(struct rb_record_queue *queue);

#endif /* RECORD_QUEUE_H_ */
