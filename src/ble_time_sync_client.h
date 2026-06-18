#ifndef BLE_TIME_SYNC_CLIENT_H
#define BLE_TIME_SYNC_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

struct ble_time_sync_client_scan_stats {
	uint32_t scan_seen;
	uint32_t scan_match;
	uint32_t scan_reject_type;
	uint32_t scan_reject_filter;
	uint32_t connect_attempts;
	uint32_t connect_failures;
	uint32_t gatt_tx_found;
	uint32_t gatt_tx_ccc_found;
	uint32_t gatt_subscribe_attempts;
	uint32_t gatt_subscribe_failures;
	uint32_t gatt_notify_subscribed;
	uint32_t gatt_rx_found;
	uint32_t gatt_write_attempts;
	uint32_t gatt_write_failures;
	uint32_t gatt_write_successes;
	uint32_t gatt_write_completions;
	uint32_t gatt_write_retries;
	uint32_t gatt_write_inflight;
	uint32_t gatt_write_step;
	int gatt_last_write_error;
};

int ble_time_sync_client_start(void);
bool ble_time_sync_client_has_peer(void);
bool ble_time_sync_client_scanning(void);
uint8_t ble_time_sync_client_peer_count(void);
bool ble_time_sync_client_notify_subscribed(void);
void ble_time_sync_client_get_scan_stats(
	struct ble_time_sync_client_scan_stats *stats);

#endif
