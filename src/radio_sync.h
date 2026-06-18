#ifndef RADIO_SYNC_H
#define RADIO_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "sync_packet.h"

struct radio_sync_rx {
	struct sync_beacon beacon;
	uint64_t local_rx_tick;
};

enum radio_sync_rx_mode {
	RADIO_SYNC_RX_STOPPED,
	RADIO_SYNC_RX_ACQUIRE,
	RADIO_SYNC_RX_RECOVERY_WINDOW,
	RADIO_SYNC_RX_LOCKED_WINDOW,
};

struct radio_sync_window {
	uint64_t start_tick;
	uint32_t length_us;
	enum radio_sync_rx_mode mode;
};

struct radio_sync_master_config {
	uint16_t initial_seq;
	uint32_t network_id;
	uint32_t sync_interval_us;
	uint32_t pps_period_us;
	uint64_t pps_epoch_tick;
	uint32_t txen_to_address_us;
};

struct radio_sync_stats {
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t rx_crc_errors;
	uint32_t rx_decode_errors;
	uint32_t rx_address_events;
	uint32_t rx_end_events;
	uint32_t rx_bad_length_events;
	uint32_t busy_errors;
	uint32_t tx_api_busy_errors;
	uint32_t tx_mpsl_request_errors;
	uint32_t tx_mpsl_blocked;
	uint32_t tx_mpsl_cancelled;
	uint32_t tx_timeout_errors;
	uint32_t tx_timing_errors;
	uint32_t tx_prepare_late;
	uint32_t tx_sequence;
	int32_t last_tx_error_us;
	uint32_t last_tx_ref_age_us;
	uint8_t last_tx_packet_len;
	uint8_t tx_address;
	uint8_t rx_address_mask;
	uint32_t last_rx_ref_age_us;
	uint16_t radio_frequency_mhz;
	uint16_t radio_datawhiteiv;
	uint8_t last_rx_packet_len;
	bool last_rx_crc_ok;
	uint32_t rxen_events;
	uint32_t rx_ready_events;
	uint32_t last_rx_radio_state;
	uint32_t timeslot_blocked;
	uint32_t timeslot_cancelled;
	uint32_t timeslot_overstayed;
	uint32_t rx_windows;
	uint32_t rx_window_skips;
	uint32_t rx_window_late;
	uint32_t rx_window_length_us;
	bool rx_request_active;
	enum radio_sync_rx_mode rx_mode;
};

int radio_sync_init(uint8_t channel, uint32_t network_id);
int radio_sync_reconfigure(uint8_t channel, uint32_t network_id);
int radio_sync_send_beacon(const struct sync_beacon *beacon,
			   uint64_t *tx_tick);
int radio_sync_send_beacon_at(const struct sync_beacon *beacon,
			      uint64_t txen_tick, uint32_t txen_to_address_us,
			      uint64_t *address_tick);
int radio_sync_start_master(const struct radio_sync_master_config *cfg,
			    uint64_t first_txen_tick);
int radio_sync_stop_master(void);
int radio_sync_start_rx(void);
int radio_sync_stop_rx(void);
int radio_sync_schedule_rx_window(const struct radio_sync_window *window);
int radio_sync_poll_rx(struct radio_sync_rx *rx);
const struct radio_sync_stats *radio_sync_stats_get(void);

#endif
