#include <stddef.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "byte_ring.h"

ZTEST(byte_ring, test_write_drops_oldest_bytes)
{
	uint8_t storage[4];
	struct rb_byte_ring ring;
	uint8_t out[4];
	const uint8_t expected[] = {3, 4, 5, 6};

	rb_byte_ring_init(&ring, storage, sizeof(storage));
	zassert_equal(rb_byte_ring_write(&ring, (uint8_t[]){1, 2, 3}, 3), 3);
	zassert_equal(rb_byte_ring_write(&ring, (uint8_t[]){4, 5, 6}, 3), 3);
	zassert_equal(rb_byte_ring_read(&ring, out, sizeof(out)), 4);
	zassert_mem_equal(out, expected, sizeof(expected));
	zassert_equal(ring.dropped_bytes, 2);
}

ZTEST(byte_ring, test_oversized_write_keeps_newest_capacity)
{
	uint8_t storage[4];
	struct rb_byte_ring ring;
	uint8_t out[4];
	const uint8_t expected[] = {3, 4, 5, 6};

	rb_byte_ring_init(&ring, storage, sizeof(storage));
	rb_byte_ring_write(&ring, (uint8_t[]){1, 2, 3, 4, 5, 6}, 6);
	zassert_equal(rb_byte_ring_read(&ring, out, sizeof(out)), 4);
	zassert_mem_equal(out, expected, sizeof(expected));
	zassert_equal(ring.dropped_bytes, 2);
}

ZTEST(byte_ring, test_wrap_peek_read_and_clear)
{
	uint8_t storage[5];
	struct rb_byte_ring ring;
	uint8_t out[5];
	const uint8_t first_read[] = {1, 2, 3};
	const uint8_t expected[] = {4, 5, 6, 7, 8};

	rb_byte_ring_init(&ring, storage, sizeof(storage));
	zassert_equal(rb_byte_ring_write(&ring, (uint8_t[]){1, 2, 3, 4}, 4), 4);
	zassert_equal(rb_byte_ring_read(&ring, out, 3), 3);
	zassert_mem_equal(out, first_read, sizeof(first_read));
	zassert_equal(rb_byte_ring_write(&ring, (uint8_t[]){5, 6, 7, 8}, 4), 4);
	zassert_equal(rb_byte_ring_size(&ring), 5);
	zassert_equal(rb_byte_ring_peek(&ring, out, sizeof(out)), 5);
	zassert_mem_equal(out, expected, sizeof(expected));
	zassert_equal(rb_byte_ring_size(&ring), 5);
	zassert_equal(rb_byte_ring_read(&ring, out, sizeof(out)), 5);
	zassert_mem_equal(out, expected, sizeof(expected));

	rb_byte_ring_write(&ring, (uint8_t[]){9, 10}, 2);
	rb_byte_ring_clear(&ring);
	zassert_equal(rb_byte_ring_size(&ring), 0);
	zassert_equal(ring.dropped_bytes, 0);
}

ZTEST(byte_ring, test_null_and_zero_capacity_are_safe)
{
	uint8_t byte = 1;
	struct rb_byte_ring ring;

	rb_byte_ring_init(&ring, NULL, 0);
	zassert_equal(rb_byte_ring_write(&ring, &byte, 1), 0);
	zassert_equal(rb_byte_ring_read(&ring, &byte, 1), 0);
	zassert_equal(rb_byte_ring_peek(&ring, &byte, 1), 0);
	zassert_equal(rb_byte_ring_size(&ring), 0);
}

ZTEST_SUITE(byte_ring, NULL, NULL, NULL, NULL, NULL);
