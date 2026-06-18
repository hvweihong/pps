#ifndef BLE_TIME_SYNC_H
#define BLE_TIME_SYNC_H

#include <stdbool.h>
#include <stdint.h>

struct ble_time_sync_command {
	char text[128];
};

struct ble_time_sync_snapshot {
	bool initialized;
	bool bt_ready;
	bool started;
	bool connected;
	bool scanning;
	bool advertising;
	bool peer;
	bool notify_enabled;
	uint8_t connection_count;
	uint32_t start_attempts;
	uint32_t scan_seen;
	uint32_t scan_match;
	uint32_t scan_reject_type;
	uint32_t scan_reject_filter;
	uint32_t connect_attempts;
	uint32_t connect_failures;
	int last_error;
};

int ble_time_sync_init(void);
int ble_time_sync_start(void);
bool ble_time_sync_started(void);
bool ble_time_sync_connected(void);
uint8_t ble_time_sync_connection_count(void);
void ble_time_sync_get_snapshot(struct ble_time_sync_snapshot *snapshot);
int ble_time_sync_get_command(struct ble_time_sync_command *cmd);

#endif
