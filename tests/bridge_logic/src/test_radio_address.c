#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "radio_address.h"

struct aes_capture {
	uint8_t inputs[16][16];
	size_t calls;
};

static int copy_aes(void *context, const uint8_t key[16],
		    const uint8_t input[16], uint8_t output[16])
{
	struct aes_capture *capture = context;

	(void)key;
	memcpy(capture->inputs[capture->calls], input, 16);
	capture->calls++;
	memcpy(output, input, 16);
	return 0;
}

ZTEST(radio_address, test_address_domains_use_exact_aes_block_layout)
{
	struct aes_capture capture = {0};
	struct rb_radio_addresses addresses;
	uint8_t key[16] = {0};

	zassert_ok(rb_radio_addresses_derive(0x11223344, key, copy_aes, &capture,
					     &addresses));
	zassert_equal(capture.calls, 5);
	zassert_equal(capture.inputs[0][0], RB_PROTOCOL_VERSION);
	zassert_equal(capture.inputs[0][1], 0x01);
	zassert_equal(capture.inputs[1][1], 0x02);
	zassert_equal(capture.inputs[2][1], 0x11);
	zassert_equal(capture.inputs[3][1], 0x12);
	zassert_equal(capture.inputs[4][1], 0x13);
	for (size_t i = 0; i < capture.calls; i++) {
		zassert_equal(capture.inputs[i][2], 0);
		zassert_equal(capture.inputs[i][3], 0);
		zassert_equal(sys_get_le32(&capture.inputs[i][4]), 0x11223344);
	}
	zassert_equal(addresses.node_prefix[0], 0x11);
	zassert_equal(addresses.node_prefix[1], 0x12);
	zassert_equal(addresses.node_prefix[2], 0x13);
}

ZTEST(radio_address, test_temporary_address_context_uses_device_id)
{
	struct aes_capture capture = {0};
	uint8_t key[16] = {0};
	uint8_t first[5];
	uint8_t second[5];

	zassert_ok(rb_temporary_address_derive(7, key, 0x0102030405060708ULL,
					       copy_aes, &capture, first));
	zassert_ok(rb_temporary_address_derive(7, key, 0x1112131415161718ULL,
					       copy_aes, &capture, second));
	zassert_equal(capture.inputs[0][1], 0x20);
	zassert_equal(sys_get_le64(&capture.inputs[0][8]),
		      0x0102030405060708ULL);
	zassert_not_equal(memcmp(first, second, sizeof(first)), 0);
}

ZTEST(radio_address, test_discovery_slot_uses_nonce_and_device_block)
{
	struct aes_capture capture = {0};
	uint8_t key[16] = {0};
	uint8_t slot = 0xff;
	const uint8_t expected[16] = {
		1, 0x30, 0, 0, 0x44, 0x33, 0x22, 0x11,
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};

	zassert_ok(rb_discovery_slot(key, 0x11223344,
				     0x0102030405060708ULL, copy_aes,
				     &capture, &slot));
	zassert_mem_equal(capture.inputs[0], expected, sizeof(expected));
	zassert_equal(slot, sys_get_le32(expected) % RB_DISCOVERY_SLOT_COUNT);
}

static int fail_aes(void *context, const uint8_t key[16],
		    const uint8_t input[16], uint8_t output[16])
{
	(void)context;
	(void)key;
	(void)input;
	(void)output;
	return -EIO;
}

ZTEST(radio_address, test_discovery_slot_propagates_aes_failure)
{
	uint8_t key[16] = {0};
	uint8_t slot = 0xaa;

	zassert_equal(rb_discovery_slot(key, 1, 2, fail_aes, NULL, &slot), -EIO);
	zassert_equal(slot, 0xaa);
}

struct retry_aes_context {
	size_t calls;
	uint8_t counters[2];
};

static int retry_aes(void *context, const uint8_t key[16],
		     const uint8_t input[16], uint8_t output[16])
{
	struct retry_aes_context *retry = context;

	(void)key;
	retry->counters[retry->calls] = input[2];
	memset(output, retry->calls == 0 ? 0x00 : 0xa5, 16);
	retry->calls++;
	return 0;
}

static int invalid_msb_aes(void *context, const uint8_t key[16],
			   const uint8_t input[16], uint8_t output[16])
{
	(void)context;
	(void)key;
	memset(output, 0, 16);
	output[0] = input[2] == 0 ? 0x55 : 0x12;
	output[1] = input[1] == 0x01 ? 0x01 : input[1];
	output[2] = 0x23;
	output[3] = 0x45;
	output[4] = 0x10;
	return 0;
}

ZTEST(radio_address, test_base_most_significant_byte_uses_esb_order)
{
	struct rb_radio_addresses addresses;
	uint8_t key[16] = {0};

	zassert_ok(rb_radio_addresses_derive(1, key, invalid_msb_aes, NULL,
					     &addresses));
	zassert_equal(addresses.base0[0], 0x12);
}

ZTEST(radio_address, test_invalid_address_retries_next_counter)
{
	struct retry_aes_context retry = {0};
	uint8_t key[16] = {0};
	uint8_t address[5];

	zassert_ok(rb_temporary_address_derive(1, key, 2, retry_aes, &retry,
					       address));
	zassert_equal(retry.calls, 2);
	zassert_equal(retry.counters[0], 0);
	zassert_equal(retry.counters[1], 1);
	for (size_t i = 0; i < sizeof(address); i++) {
		zassert_equal(address[i], 0xa5);
	}
}

ZTEST_SUITE(radio_address, NULL, NULL, NULL, NULL, NULL);
