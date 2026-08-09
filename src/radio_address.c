#include "radio_address.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define RB_DOMAIN_BROADCAST 0x01u
#define RB_DOMAIN_BASE1 0x02u
#define RB_DOMAIN_NODE_FIRST 0x11u
#define RB_DOMAIN_TEMPORARY 0x20u
#define RB_DOMAIN_DISCOVERY_SLOT 0x30u
#define RB_DERIVATION_ATTEMPTS 256u

static void derivation_block(uint8_t domain, uint8_t counter,
			     uint32_t group_or_nonce, uint64_t context,
			     uint8_t block[16])
{
	memset(block, 0, 16);
	block[0] = RB_PROTOCOL_VERSION;
	block[1] = domain;
	block[2] = counter;
	sys_put_le32(group_or_nonce, &block[4]);
	sys_put_le64(context, &block[8]);
}

static int derive(uint8_t domain, uint8_t counter, uint32_t group_or_nonce,
		  uint64_t context, const uint8_t key[16], rb_aes128_fn aes,
		  void *aes_context, uint8_t output[16])
{
	uint8_t input[16];

	derivation_block(domain, counter, group_or_nonce, context, input);
	return aes(aes_context, key, input, output);
}

/* nRF52 RADIO address recommendations and anomaly 107 constraints. */
static bool radio_address_valid(const uint8_t base[4], uint8_t prefix)
{
	bool all_zero = prefix == 0;
	bool all_one = prefix == UINT8_MAX;

	for (size_t i = 0; i < 4; i++) {
		all_zero = all_zero && base[i] == 0;
		all_one = all_one && base[i] == UINT8_MAX;
	}

	if (all_zero || all_one) {
		return false;
	}

	/* ESB accepts the base in big-endian byte order. */
	if (base[0] == 0x55 || base[0] == 0xaa) {
		return false;
	}

	/* Anomaly 107: prefix and the two least-significant base bytes cannot
	 * all be zero for pipe 0. Applying it to every derived address keeps a
	 * temporary address safe when it is installed as pipe 0.
	 */
	return prefix != 0 || base[2] != 0 || base[3] != 0;
}

static bool prefixes_unique(const uint8_t prefixes[3])
{
	return prefixes[0] != prefixes[1] && prefixes[0] != prefixes[2] &&
	       prefixes[1] != prefixes[2];
}

int rb_radio_addresses_derive(uint32_t group_id, const uint8_t key[16],
			      rb_aes128_fn aes, void *aes_context,
			      struct rb_radio_addresses *addresses)
{
	if (key == NULL || aes == NULL || addresses == NULL) {
		return -EINVAL;
	}

	for (uint16_t counter = 0; counter < RB_DERIVATION_ATTEMPTS; counter++) {
		struct rb_radio_addresses candidate;
		uint8_t output[16];
		int err;

		err = derive(RB_DOMAIN_BROADCAST, (uint8_t)counter, group_id, 0,
			     key, aes, aes_context, output);
		if (err != 0) {
			return err;
		}
		memcpy(candidate.base0, output, sizeof(candidate.base0));
		candidate.prefix0 = output[4];

		err = derive(RB_DOMAIN_BASE1, (uint8_t)counter, group_id, 0, key,
			     aes, aes_context, output);
		if (err != 0) {
			return err;
		}
		memcpy(candidate.base1, output, sizeof(candidate.base1));

		for (uint8_t i = 0; i < 3; i++) {
			err = derive((uint8_t)(RB_DOMAIN_NODE_FIRST + i),
				     (uint8_t)counter, group_id, (uint64_t)i + 1u,
				     key, aes, aes_context, output);
			if (err != 0) {
				return err;
			}
			candidate.node_prefix[i] = output[1];
		}

		if (!radio_address_valid(candidate.base0, candidate.prefix0) ||
		    !prefixes_unique(candidate.node_prefix)) {
			continue;
		}

		bool nodes_valid = true;
		for (size_t i = 0; i < 3; i++) {
			nodes_valid = nodes_valid &&
				radio_address_valid(candidate.base1,
						    candidate.node_prefix[i]);
		}
		if (nodes_valid) {
			*addresses = candidate;
			return 0;
		}
	}

	return -ERANGE;
}

int rb_temporary_address_derive(uint32_t group_id, const uint8_t key[16],
				uint64_t device_id, rb_aes128_fn aes,
				void *aes_context, uint8_t address[5])
{
	if (key == NULL || aes == NULL || address == NULL) {
		return -EINVAL;
	}

	for (uint16_t counter = 0; counter < RB_DERIVATION_ATTEMPTS; counter++) {
		uint8_t output[16];
		uint8_t candidate[5];
		int err = derive(RB_DOMAIN_TEMPORARY, (uint8_t)counter, group_id,
				 device_id, key, aes, aes_context, output);

		if (err != 0) {
			return err;
		}
		memcpy(candidate, &output[8], sizeof(candidate));
		if (radio_address_valid(candidate, candidate[4])) {
			memcpy(address, candidate, sizeof(candidate));
			return 0;
		}
	}

	return -ERANGE;
}

int rb_discovery_slot(const uint8_t key[16], uint32_t nonce,
		      uint64_t device_id, rb_aes128_fn aes, void *aes_context,
		      uint8_t *slot)
{
	uint8_t output[16];
	int err;

	if (key == NULL || aes == NULL || slot == NULL) {
		return -EINVAL;
	}

	err = derive(RB_DOMAIN_DISCOVERY_SLOT, 0, nonce, device_id, key, aes,
		     aes_context, output);
	if (err != 0) {
		return err;
	}

	*slot = (uint8_t)(sys_get_le32(output) % RB_DISCOVERY_SLOT_COUNT);
	return 0;
}
