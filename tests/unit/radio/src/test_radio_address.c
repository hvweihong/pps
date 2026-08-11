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

static int fail_aes(void *context, const uint8_t key[16],
		    const uint8_t input[16], uint8_t output[16])
{
	(void)context;
	(void)key;
	(void)input;
	(void)output;
	return -EIO;
}

ZTEST(radio_address, test_address_derivation_propagates_aes_failure)
{
	struct rb_radio_addresses addresses;
	uint8_t key[16] = {0};

	zassert_equal(rb_radio_addresses_derive(1u, key, fail_aes, NULL,
						 &addresses), -EIO);
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

ZTEST_SUITE(radio_address, NULL, NULL, NULL, NULL, NULL);
