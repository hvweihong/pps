#include "record_queue.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

void rb_record_queue_init(struct rb_record_queue *queue,
			  uint8_t *storage, size_t storage_size,
			  uint16_t *lengths, size_t length_capacity)
{
	if (queue == NULL) {
		return;
	}
	memset(queue, 0, sizeof(*queue));
	if (storage == NULL || storage_size == 0u || lengths == NULL ||
	    length_capacity == 0u) {
		return;
	}
	queue->storage = storage;
	queue->storage_size = storage_size;
	queue->lengths = lengths;
	queue->length_capacity = length_capacity;
}

int rb_record_queue_push(struct rb_record_queue *queue,
			 const uint8_t *data, size_t len)
{
	if (queue == NULL || queue->storage == NULL || queue->lengths == NULL ||
	    data == NULL || len == 0u) {
		return -EINVAL;
	}
	if (len > UINT16_MAX) {
		return -EMSGSIZE;
	}
	if (queue->length_count == queue->length_capacity ||
	    len > queue->storage_size - queue->data_count) {
		return -ENOSPC;
	}
	for (size_t i = 0u; i < len; i++) {
		queue->storage[(queue->data_tail + i) % queue->storage_size] = data[i];
	}
	queue->data_tail = (queue->data_tail + len) % queue->storage_size;
	queue->data_count += len;
	queue->lengths[queue->length_tail] = (uint16_t)len;
	queue->length_tail = (queue->length_tail + 1u) % queue->length_capacity;
	queue->length_count++;
	return 0;
}

int rb_record_queue_peek(const struct rb_record_queue *queue,
			 uint8_t *data, size_t max_len, size_t *record_len)
{
	size_t len;

	if (queue == NULL || queue->storage == NULL || queue->lengths == NULL ||
	    data == NULL || record_len == NULL) {
		return -EINVAL;
	}
	if (queue->length_count == 0u) {
		return -EAGAIN;
	}
	len = queue->lengths[queue->length_head];
	if (len > max_len) {
		return -EMSGSIZE;
	}
	for (size_t i = 0u; i < len; i++) {
		data[i] = queue->storage[(queue->data_head + i) % queue->storage_size];
	}
	*record_len = len;
	return 0;
}

int rb_record_queue_pop(struct rb_record_queue *queue)
{
	size_t len;

	if (queue == NULL || queue->storage == NULL || queue->lengths == NULL) {
		return -EINVAL;
	}
	if (queue->length_count == 0u) {
		return -EAGAIN;
	}
	len = queue->lengths[queue->length_head];
	queue->data_head = (queue->data_head + len) % queue->storage_size;
	queue->data_count -= len;
	queue->length_head = (queue->length_head + 1u) % queue->length_capacity;
	queue->length_count--;
	return 0;
}
