#ifndef BLE_TIME_SYNC_SERVICE_H
#define BLE_TIME_SYNC_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*ble_time_sync_command_handler_t)(const char *cmd, size_t len);

int ble_time_sync_service_init(ble_time_sync_command_handler_t handler);
int ble_time_sync_service_notify(const char *msg);
bool ble_time_sync_service_notify_enabled(void);
const char *ble_time_sync_service_status(void);

#endif
