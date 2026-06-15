#include "sync_packet.h"

static void put_u16(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)value;
	buf[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *buf, uint32_t value)
{
	for (int i = 0; i < 4; i++) {
		buf[i] = (uint8_t)(value >> (8 * i));
	}
}

static void put_u64(uint8_t *buf, uint64_t value)
{
	for (int i = 0; i < 8; i++) {
		buf[i] = (uint8_t)(value >> (8 * i));
	}
}

static uint16_t get_u16(const uint8_t *buf)
{
	return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t get_u32(const uint8_t *buf)
{
	uint32_t value = 0;

	for (int i = 0; i < 4; i++) {
		value |= ((uint32_t)buf[i]) << (8 * i);
	}

	return value;
}

static uint64_t get_u64(const uint8_t *buf)
{
	uint64_t value = 0;

	for (int i = 0; i < 8; i++) {
		value |= ((uint64_t)buf[i]) << (8 * i);
	}

	return value;
}

static uint32_t crc32_ieee(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int bit = 0; bit < 8; bit++) {
			uint32_t mask = -(crc & 1u);

			crc = (crc >> 1) ^ (0xedb88320u & mask);
		}
	}

	return ~crc;
}

int sync_beacon_encode(const struct sync_beacon *beacon, uint8_t *buf,
		       size_t len)
{
	uint32_t crc;

	if (beacon == NULL || buf == NULL) {
		return -EINVAL;
	}

	if (len < SYNC_BEACON_WIRE_SIZE) {
		return -ENOSPC;
	}

	put_u32(&buf[0], SYNC_BEACON_MAGIC);
	buf[4] = SYNC_BEACON_VERSION;
	buf[5] = beacon->role;
	put_u16(&buf[6], beacon->seq);
	put_u32(&buf[8], beacon->network_id);
	put_u64(&buf[12], beacon->master_tx_tick);
	put_u64(&buf[20], beacon->next_pps_master_tick);
	put_u32(&buf[28], beacon->sync_interval_us);
	put_u32(&buf[32], beacon->status_flags);
	put_u32(&buf[36], 0);

	crc = crc32_ieee(buf, SYNC_BEACON_WIRE_SIZE - 4);
	put_u32(&buf[40], crc);

	return 0;
}

int sync_beacon_decode(const uint8_t *buf, size_t len, uint32_t network_id,
		       struct sync_beacon *beacon)
{
	uint32_t expected_crc;
	uint32_t actual_crc;

	if (beacon == NULL || buf == NULL) {
		return -EINVAL;
	}

	if (len != SYNC_BEACON_WIRE_SIZE) {
		return -EMSGSIZE;
	}

	expected_crc = get_u32(&buf[40]);
	actual_crc = crc32_ieee(buf, SYNC_BEACON_WIRE_SIZE - 4);
	if (expected_crc != actual_crc) {
		return -EBADMSG;
	}

	beacon->magic = get_u32(&buf[0]);
	beacon->version = buf[4];
	beacon->role = buf[5];
	beacon->seq = get_u16(&buf[6]);
	beacon->network_id = get_u32(&buf[8]);
	beacon->master_tx_tick = get_u64(&buf[12]);
	beacon->next_pps_master_tick = get_u64(&buf[20]);
	beacon->sync_interval_us = get_u32(&buf[28]);
	beacon->status_flags = get_u32(&buf[32]);
	beacon->crc = expected_crc;

	if (beacon->magic != SYNC_BEACON_MAGIC ||
	    beacon->version != SYNC_BEACON_VERSION ||
	    beacon->role != SYNC_BEACON_ROLE_MASTER) {
		return -EBADMSG;
	}

	if (beacon->network_id != network_id) {
		return -EACCES;
	}

	return 0;
}
