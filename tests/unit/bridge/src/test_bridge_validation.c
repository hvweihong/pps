#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "bridge_validation.h"

ZTEST(bridge_validation, test_pattern_round_trip_across_chunk_boundaries)
{
	struct rb_validation_pipe pipe;
	uint8_t chunk[257];
	size_t offset = 0u;

	rb_validation_pipe_init(&pipe);
	zassert_ok(rb_validation_inject_pattern(&pipe, 600u, 0x31u));
	while (offset < 600u) {
		size_t len = rb_validation_pull_input(&pipe, chunk, sizeof(chunk));

		zassert_true(len != 0u);
		for (size_t i = 0; i < len; i++) {
			zassert_equal(chunk[i], (uint8_t)(0x31u + offset + i));
		}
		offset += len;
	}
	zassert_equal(rb_validation_pull_input(&pipe, chunk, sizeof(chunk)), 0u);
}

ZTEST(bridge_validation, test_capture_verify_is_exact_and_non_destructive_on_error)
{
	struct rb_validation_pipe pipe;
	uint8_t payload[600];
	uint8_t preview[37];
	size_t mismatch = 0u;

	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(0x72u + i);
	}
	rb_validation_pipe_init(&pipe);
	zassert_equal(rb_validation_capture_output(&pipe, payload, 211u), 211u);
	zassert_equal(rb_validation_capture_output(&pipe, payload + 211u,
						   sizeof(payload) - 211u),
		      sizeof(payload) - 211u);
	zassert_equal(rb_validation_copy_output(&pipe, 0u, preview, sizeof(preview)),
		      sizeof(preview));
	zassert_mem_equal(preview, payload, sizeof(preview));
	zassert_equal(rb_validation_output_size(&pipe), sizeof(payload));
	zassert_equal(rb_validation_verify_output(&pipe, sizeof(payload), 0x71u,
						  &mismatch), -EBADMSG);
	zassert_equal(mismatch, 0u);
	zassert_equal(rb_validation_output_size(&pipe), sizeof(payload));
	zassert_ok(rb_validation_verify_output(&pipe, sizeof(payload), 0x72u,
						 &mismatch));
	zassert_equal(rb_validation_output_size(&pipe), 0u);
}

ZTEST(bridge_validation, test_injection_fails_without_dropping_queued_bytes)
{
	struct rb_validation_pipe pipe;
	uint8_t byte;

	rb_validation_pipe_init(&pipe);
	zassert_ok(rb_validation_inject_pattern(&pipe,
					 RB_VALIDATION_BUFFER_SIZE, 0x10u));
	zassert_equal(rb_validation_inject_pattern(&pipe, 1u, 0x20u), -ENOSPC);
	zassert_equal(rb_validation_input_size(&pipe), RB_VALIDATION_BUFFER_SIZE);
	zassert_equal(rb_validation_pull_input(&pipe, &byte, 1u), 1u);
	zassert_equal(byte, 0x10u);
}

ZTEST(bridge_validation, test_output_overflow_preserves_existing_bytes_and_fails_verify)
{
	static uint8_t payload[RB_VALIDATION_BUFFER_SIZE - 4u];
	const uint8_t overflow[] = {1u, 2u, 3u, 4u, 5u};
	struct rb_validation_stats stats;
	struct rb_validation_pipe pipe;
	uint8_t preview[8];
	size_t mismatch = SIZE_MAX;

	for (size_t i = 0u; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(0x20u + i);
	}
	rb_validation_pipe_init(&pipe);
	zassert_equal(rb_validation_capture_output(&pipe, payload, sizeof(payload)),
		      sizeof(payload));
	zassert_equal(rb_validation_capture_output(&pipe, overflow,
					   sizeof(overflow)), 0u);
	zassert_equal(rb_validation_output_size(&pipe), sizeof(payload));
	zassert_equal(rb_validation_copy_output(&pipe, 0u, preview, sizeof(preview)),
		      sizeof(preview));
	zassert_mem_equal(preview, payload, sizeof(preview));
	rb_validation_stats_get(&pipe, &stats);
	zassert_equal(stats.captured_bytes, sizeof(payload));
	zassert_equal(stats.output_drop_bytes, sizeof(overflow));
	zassert_equal(rb_validation_verify_output(&pipe, sizeof(preview), 0x20u,
					  &mismatch), -EOVERFLOW);
	zassert_equal(rb_validation_output_size(&pipe), sizeof(payload));
}

ZTEST_SUITE(bridge_validation, NULL, NULL, NULL, NULL, NULL);
