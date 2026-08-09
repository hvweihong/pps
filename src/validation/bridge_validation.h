#ifndef BRIDGE_VALIDATION_H_
#define BRIDGE_VALIDATION_H_

#include <stddef.h>
#include <stdint.h>

#include "byte_ring.h"

#define RB_VALIDATION_BUFFER_SIZE 2048u

struct rb_validation_pipe {
	struct rb_byte_ring input;
	struct rb_byte_ring output;
	uint8_t input_storage[RB_VALIDATION_BUFFER_SIZE];
	uint8_t output_storage[RB_VALIDATION_BUFFER_SIZE];
	uint64_t injected_bytes;
	uint64_t captured_bytes;
};

struct rb_validation_stats {
	size_t input_bytes;
	size_t output_bytes;
	uint64_t injected_bytes;
	uint64_t captured_bytes;
	uint64_t input_drop_bytes;
	uint64_t output_drop_bytes;
};

void rb_validation_pipe_init(struct rb_validation_pipe *pipe);
int rb_validation_inject_pattern(struct rb_validation_pipe *pipe, size_t len,
				 uint8_t seed);
size_t rb_validation_pull_input(struct rb_validation_pipe *pipe, uint8_t *data,
				size_t max_len);
size_t rb_validation_capture_output(struct rb_validation_pipe *pipe,
				    const uint8_t *data, size_t len);
size_t rb_validation_copy_output(const struct rb_validation_pipe *pipe,
				 size_t offset, uint8_t *data, size_t max_len);
int rb_validation_verify_output(struct rb_validation_pipe *pipe, size_t len,
				uint8_t seed, size_t *mismatch_offset);
size_t rb_validation_input_size(const struct rb_validation_pipe *pipe);
size_t rb_validation_output_size(const struct rb_validation_pipe *pipe);
void rb_validation_clear(struct rb_validation_pipe *pipe);
void rb_validation_stats_get(const struct rb_validation_pipe *pipe,
			     struct rb_validation_stats *stats);

#endif /* BRIDGE_VALIDATION_H_ */
