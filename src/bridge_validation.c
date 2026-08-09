#include "bridge_validation.h"

#include <errno.h>
#include <string.h>

void rb_validation_pipe_init(struct rb_validation_pipe *pipe)
{
	if (pipe == NULL) {
		return;
	}
	memset(pipe, 0, sizeof(*pipe));
	rb_byte_ring_init(&pipe->input, pipe->input_storage,
			  sizeof(pipe->input_storage));
	rb_byte_ring_init(&pipe->output, pipe->output_storage,
			  sizeof(pipe->output_storage));
}

int rb_validation_inject_pattern(struct rb_validation_pipe *pipe, size_t len,
				 uint8_t seed)
{
	uint8_t chunk[64];
	size_t offset = 0u;

	if (pipe == NULL || len == 0u) {
		return -EINVAL;
	}
	if (len > pipe->input.capacity - rb_byte_ring_size(&pipe->input)) {
		return -ENOSPC;
	}
	while (offset < len) {
		size_t chunk_len = len - offset;

		if (chunk_len > sizeof(chunk)) {
			chunk_len = sizeof(chunk);
		}
		for (size_t i = 0u; i < chunk_len; i++) {
			chunk[i] = (uint8_t)(seed + offset + i);
		}
		(void)rb_byte_ring_write(&pipe->input, chunk, chunk_len);
		offset += chunk_len;
	}
	pipe->injected_bytes += len;
	return 0;
}

size_t rb_validation_pull_input(struct rb_validation_pipe *pipe, uint8_t *data,
				size_t max_len)
{
	if (pipe == NULL) {
		return 0u;
	}
	return rb_byte_ring_read(&pipe->input, data, max_len);
}

size_t rb_validation_capture_output(struct rb_validation_pipe *pipe,
				    const uint8_t *data, size_t len)
{
	size_t captured;
	size_t available;

	if (pipe == NULL || data == NULL || len == 0u ||
	    pipe->output.storage == NULL || pipe->output.capacity == 0u) {
		return 0u;
	}
	available = pipe->output.capacity - rb_byte_ring_size(&pipe->output);
	if (len > available) {
		pipe->output.dropped_bytes += len;
		return 0u;
	}
	captured = rb_byte_ring_write(&pipe->output, data, len);
	pipe->captured_bytes += captured;
	return captured;
}

size_t rb_validation_copy_output(const struct rb_validation_pipe *pipe,
				 size_t offset, uint8_t *data, size_t max_len)
{
	size_t count;

	if (pipe == NULL || data == NULL || max_len == 0u ||
	    offset >= rb_byte_ring_size(&pipe->output)) {
		return 0u;
	}
	count = rb_byte_ring_size(&pipe->output) - offset;
	if (count > max_len) {
		count = max_len;
	}
	for (size_t i = 0u; i < count; i++) {
		size_t index = (pipe->output.read_index + offset + i) %
			pipe->output.capacity;

		data[i] = pipe->output.storage[index];
	}
	return count;
}

int rb_validation_verify_output(struct rb_validation_pipe *pipe, size_t len,
				uint8_t seed, size_t *mismatch_offset)
{
	if (pipe == NULL || len == 0u) {
		return -EINVAL;
	}
	if (pipe->output.dropped_bytes != 0u) {
		if (mismatch_offset != NULL) {
			*mismatch_offset = 0u;
		}
		return -EOVERFLOW;
	}
	if (rb_byte_ring_size(&pipe->output) < len) {
		return -EAGAIN;
	}
	for (size_t i = 0u; i < len; i++) {
		size_t index = (pipe->output.read_index + i) % pipe->output.capacity;
		uint8_t expected = (uint8_t)(seed + i);

		if (pipe->output.storage[index] != expected) {
			if (mismatch_offset != NULL) {
				*mismatch_offset = i;
			}
			return -EBADMSG;
		}
	}
	(void)rb_byte_ring_discard(&pipe->output, len);
	if (mismatch_offset != NULL) {
		*mismatch_offset = len;
	}
	return 0;
}

size_t rb_validation_input_size(const struct rb_validation_pipe *pipe)
{
	return pipe == NULL ? 0u : rb_byte_ring_size(&pipe->input);
}

size_t rb_validation_output_size(const struct rb_validation_pipe *pipe)
{
	return pipe == NULL ? 0u : rb_byte_ring_size(&pipe->output);
}

void rb_validation_clear(struct rb_validation_pipe *pipe)
{
	if (pipe == NULL) {
		return;
	}
	rb_byte_ring_clear(&pipe->input);
	rb_byte_ring_clear(&pipe->output);
	pipe->injected_bytes = 0u;
	pipe->captured_bytes = 0u;
}

void rb_validation_stats_get(const struct rb_validation_pipe *pipe,
			     struct rb_validation_stats *stats)
{
	if (pipe == NULL || stats == NULL) {
		return;
	}
	*stats = (struct rb_validation_stats){
		.input_bytes = rb_byte_ring_size(&pipe->input),
		.output_bytes = rb_byte_ring_size(&pipe->output),
		.injected_bytes = pipe->injected_bytes,
		.captured_bytes = pipe->captured_bytes,
		.input_drop_bytes = pipe->input.dropped_bytes,
		.output_drop_bytes = pipe->output.dropped_bytes,
	};
}
