#ifndef RADIO_ADDRESS_H_
#define RADIO_ADDRESS_H_

#include <stdint.h>

#include "link_protocol.h"

#define RB_DISCOVERY_SLOT_COUNT 8u

typedef int (*rb_aes128_fn)(void *context, const uint8_t key[16],
			    const uint8_t input[16], uint8_t output[16]);

struct rb_radio_addresses {
	uint8_t base0[4];
	uint8_t prefix0;
	uint8_t base1[4];
	uint8_t node_prefix[3];
};

int rb_radio_addresses_derive(uint32_t group_id, const uint8_t key[16],
			      rb_aes128_fn aes, void *aes_context,
			      struct rb_radio_addresses *addresses);
int rb_temporary_address_derive(uint32_t group_id, const uint8_t key[16],
				uint64_t device_id, rb_aes128_fn aes,
				void *aes_context, uint8_t address[5]);
int rb_discovery_slot(const uint8_t key[16], uint32_t nonce,
		      uint64_t device_id, rb_aes128_fn aes, void *aes_context,
		      uint8_t *slot);

#endif /* RADIO_ADDRESS_H_ */
