#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdint.h>

struct time_sync_runtime_config {
	uint32_t network_id;
	uint8_t rf_channel;
	uint32_t sync_interval_us;
};

void runtime_config_init(void);
struct time_sync_runtime_config runtime_config_get(void);
int runtime_config_set(const struct time_sync_runtime_config *cfg);

#endif
