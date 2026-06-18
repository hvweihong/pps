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
};

int ble_time_sync_client_start(void);
bool ble_time_sync_client_has_peer(void);
bool ble_time_sync_client_scanning(void);
void ble_time_sync_client_get_scan_stats(
	struct ble_time_sync_client_scan_stats *stats);

#endif
