#include "radio_address_crypto.h"

#include <errno.h>
#include <string.h>

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

struct rb_ecb_session {
	struct cipher_ctx context;
	const struct device *device;
};

static int ecb_encrypt(void *context, const uint8_t key[16],
		       const uint8_t input[16], uint8_t output[16])
{
	struct rb_ecb_session *session = context;
	struct cipher_pkt packet = {
		.in_buf = (uint8_t *)input,
		.in_len = 16,
		.out_buf = output,
		.out_buf_max = 16,
	};
	int err;

	(void)key;
	err = cipher_block_op(&session->context, &packet);
	return err == 0 && packet.out_len == 16 ? 0 : (err != 0 ? err : -EIO);
}

static int with_session(const uint8_t key[16],
			int (*derive)(rb_aes128_fn, void *context, void *output),
			void *output)
{
	struct rb_ecb_session session = {
		.context = {
			.keylen = 16,
			.key.bit_stream = (uint8_t *)key,
			.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
		},
	};
	int err;

	if (key == NULL || derive == NULL || output == NULL) {
		return -EINVAL;
	}

	session.device = DEVICE_DT_GET_ONE(nordic_nrf_ecb);
	if (!device_is_ready(session.device)) {
		return -ENODEV;
	}
	err = cipher_begin_session(session.device, &session.context,
				   CRYPTO_CIPHER_ALGO_AES,
				   CRYPTO_CIPHER_MODE_ECB,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (err != 0) {
		return err;
	}
	err = derive(ecb_encrypt, &session, output);
	(void)cipher_free_session(session.device, &session.context);
	return err;
}

struct addresses_args {
	uint32_t group_id;
	const uint8_t *key;
	struct rb_radio_addresses *addresses;
};

static int derive_addresses(rb_aes128_fn aes, void *context, void *output)
{
	struct addresses_args *args = output;
	return rb_radio_addresses_derive(args->group_id, args->key, aes, context,
					args->addresses);
}

int rb_radio_addresses_derive_nrf(uint32_t group_id, const uint8_t key[16],
				  struct rb_radio_addresses *addresses)
{
	struct addresses_args args = {group_id, key, addresses};
	return with_session(key, derive_addresses, &args);
}
