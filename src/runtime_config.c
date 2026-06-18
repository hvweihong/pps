#include "runtime_config.h"

#include "app_config.h"

#include <errno.h>

#include <zephyr/irq.h>

static struct time_sync_runtime_config current_cfg;

void runtime_config_init(void)
{
	current_cfg.network_id = TIME_SYNC_NETWORK_ID_VALUE;
	current_cfg.rf_channel = TIME_SYNC_RF_CHANNEL_VALUE;
	current_cfg.sync_interval_us = TIME_SYNC_INTERVAL_US_VALUE;
}

struct time_sync_runtime_config runtime_config_get(void)
{
	struct time_sync_runtime_config snapshot;
	unsigned int key = irq_lock();

	snapshot = current_cfg;
	irq_unlock(key);

	return snapshot;
}

int runtime_config_set(const struct time_sync_runtime_config *cfg)
{
	unsigned int key;

	if (cfg == NULL || cfg->rf_channel > 100u ||
	    cfg->sync_interval_us < 10000u ||
	    cfg->sync_interval_us > 1000000u) {
		return -EINVAL;
	}

	key = irq_lock();
	current_cfg = *cfg;
	irq_unlock(key);

	return 0;
}
