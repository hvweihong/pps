#ifndef RADIO_SYNC_H
#define RADIO_SYNC_H

#include <stdint.h>

#include "sync_packet.h"

struct radio_sync_rx {
	struct sync_beacon beacon;
	uint64_t local_rx_tick;
};

struct radio_sync_stats {
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t rx_crc_errors;
	uint32_t rx_decode_errors;
	uint32_t busy_errors;
};

int radio_sync_init(uint8_t channel, uint32_t network_id);
int radio_sync_send_beacon(const struct sync_beacon *beacon,
			   uint64_t *tx_tick);
int radio_sync_send_beacon_at(const struct sync_beacon *beacon,
			      uint64_t txen_tick, uint32_t txen_to_address_us,
			      uint64_t *address_tick);
int radio_sync_start_rx(void);
int radio_sync_poll_rx(struct radio_sync_rx *rx);
const struct radio_sync_stats *radio_sync_stats_get(void);

#endif
