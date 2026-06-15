#ifndef SYNC_PACKET_H
#define SYNC_PACKET_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define SYNC_BEACON_MAGIC 0x54534e52u
#define SYNC_BEACON_VERSION 1u
#define SYNC_BEACON_ROLE_MASTER 1u
#define SYNC_BEACON_WIRE_SIZE 44u

struct sync_beacon {
	uint32_t magic;
	uint8_t version;
	uint8_t role;
	uint16_t seq;
	uint32_t network_id;
	uint64_t master_tx_tick;
	uint64_t next_pps_master_tick;
	uint32_t sync_interval_us;
	uint32_t status_flags;
	uint32_t crc;
};

int sync_beacon_encode(const struct sync_beacon *beacon, uint8_t *buf,
		       size_t len);
int sync_beacon_decode(const uint8_t *buf, size_t len, uint32_t network_id,
		       struct sync_beacon *beacon);

#endif
