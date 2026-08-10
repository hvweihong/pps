#ifndef RADIO_ADDRESS_CRYPTO_H_
#define RADIO_ADDRESS_CRYPTO_H_

#include <stdint.h>

#include "radio_address.h"

int rb_radio_addresses_derive_nrf(uint32_t group_id, const uint8_t key[16],
				  struct rb_radio_addresses *addresses);
int rb_temporary_address_derive_nrf(uint32_t group_id, const uint8_t key[16],
				    uint64_t device_id, uint8_t address[5]);
int rb_discovery_slot_nrf(const uint8_t key[16], uint32_t nonce,
				  uint64_t device_id, uint8_t *slot);

#endif /* RADIO_ADDRESS_CRYPTO_H_ */
