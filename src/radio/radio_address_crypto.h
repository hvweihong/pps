#ifndef RADIO_ADDRESS_CRYPTO_H_
#define RADIO_ADDRESS_CRYPTO_H_

#include <stdint.h>

#include "radio_address.h"

int rb_radio_addresses_derive_nrf(uint32_t group_id, const uint8_t key[16],
				  struct rb_radio_addresses *addresses);

#endif /* RADIO_ADDRESS_CRYPTO_H_ */
