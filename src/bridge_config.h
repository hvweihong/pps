#ifndef BRIDGE_CONFIG_H_
#define BRIDGE_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#define RB_GROUP_KEY_BYTES 16u
#define RB_CHANNEL_COUNT 40u

bool rb_group_id_valid(uint32_t group_id);
uint8_t rb_channel_index(uint32_t group_id);
uint16_t rb_channel_frequency_mhz(uint32_t group_id);
int rb_group_key_parse(const char *hex, uint8_t key[RB_GROUP_KEY_BYTES]);

#endif /* BRIDGE_CONFIG_H_ */
