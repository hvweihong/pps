#include "bridge_config.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

bool rb_group_id_valid(uint32_t group_id)
{
	return group_id != 0 && group_id != UINT32_MAX;
}

uint8_t rb_channel_index(uint32_t group_id)
{
	return (uint8_t)(group_id % RB_CHANNEL_COUNT);
}

uint16_t rb_channel_frequency_mhz(uint32_t group_id)
{
	return 2402u + (2u * rb_channel_index(group_id));
}

static int hex_nibble(char character)
{
	if (character >= '0' && character <= '9') {
		return character - '0';
	}

	if (character >= 'a' && character <= 'f') {
		return character - 'a' + 10;
	}

	if (character >= 'A' && character <= 'F') {
		return character - 'A' + 10;
	}

	return -EINVAL;
}

int rb_group_key_parse(const char *hex, uint8_t key[RB_GROUP_KEY_BYTES])
{
	if (hex == NULL || key == NULL) {
		return -EINVAL;
	}
	if (strlen(hex) != RB_GROUP_KEY_BYTES * 2u) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < RB_GROUP_KEY_BYTES; i++) {
		int high = hex_nibble(hex[i * 2]);
		int low = hex_nibble(hex[i * 2 + 1]);

		if (high < 0 || low < 0) {
			return -EINVAL;
		}

		key[i] = (uint8_t)((high << 4) | low);
	}

	return 0;
}
