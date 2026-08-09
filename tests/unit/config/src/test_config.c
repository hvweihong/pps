#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "bridge_config.h"

ZTEST(bridge_config, test_channel_index_and_frequency_follow_group_id)
{
	zassert_equal(rb_channel_index(1), 1);
	zassert_equal(rb_channel_frequency_mhz(1), 2404);
	zassert_equal(rb_channel_index(39), 39);
	zassert_equal(rb_channel_frequency_mhz(39), 2480);
	zassert_equal(rb_channel_index(40), 0);
	zassert_equal(rb_channel_frequency_mhz(40), 2402);
	zassert_equal(rb_channel_index(41), 1);
}

ZTEST(bridge_config, test_group_id_validation_rejects_reserved_values)
{
	zassert_false(rb_group_id_valid(0));
	zassert_false(rb_group_id_valid(UINT32_MAX));
	zassert_true(rb_group_id_valid(1));
	zassert_true(rb_group_id_valid(UINT32_MAX - 1));
}

ZTEST(bridge_config, test_group_key_parse_accepts_upper_and_lowercase_hex)
{
	uint8_t key[RB_GROUP_KEY_BYTES];
	const uint8_t expected[RB_GROUP_KEY_BYTES] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};

	zassert_equal(rb_group_key_parse("00112233445566778899aAbBcCdDeEfF", key), 0);
	zassert_mem_equal(key, expected, sizeof(key));
}

ZTEST(bridge_config, test_group_key_parse_rejects_invalid_input)
{
	uint8_t key[RB_GROUP_KEY_BYTES];

	zassert_equal(rb_group_key_parse(NULL, key), -EINVAL);
	zassert_equal(rb_group_key_parse("0", key), -EINVAL);
	zassert_equal(rb_group_key_parse("0011", key), -EINVAL);
	zassert_equal(rb_group_key_parse("00112233445566778899aabbccddeef", key),
		      -EINVAL);
	zassert_equal(rb_group_key_parse("00112233445566778899aabbccddeeff00", key),
		      -EINVAL);
	zassert_equal(rb_group_key_parse("00112233445566778899aabbccddeefg", key),
		      -EINVAL);
	zassert_equal(rb_group_key_parse("00112233445566778899aabbccddeeff", NULL),
		      -EINVAL);
}

ZTEST_SUITE(bridge_config, NULL, NULL, NULL, NULL, NULL);
