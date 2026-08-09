/*
 * Parameter Configuration Implementation
 *
 * Runtime parameter management with NVS persistence.
 */

#include "param_config.h"
#include "bridge_config.h"
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Default group key parsed from Kconfig at compile time */
static uint8_t default_group_key[RB_GROUP_KEY_BYTES];

LOG_MODULE_REGISTER(param_config, LOG_LEVEL_INF);

/* NVS namespace for parameters */
#define PARAM_NVS_NAMESPACE "params"

/* Runtime parameter cache */
struct param_cache_entry {
	union {
		uint32_t value;
		uint8_t bytes[32];  /* Max 32 bytes for group_key etc */
	} data;
	bool is_persisted;
};

static struct param_cache_entry param_cache[RB_PARAM_COUNT];
static bool initialized;

/* Spinlock for atomic NVS updates */
static struct k_spinlock param_lock;

/* Parameter table definition */
const struct rb_param_descriptor rb_param_table[RB_PARAM_COUNT] = {
	[RB_PARAM_ROLE_ID] = {
		.name = "role_id",
		.description = "Device role: 0=master, 1-3=slave",
		.type = RB_PARAM_UINT8,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u8 = {
			.min = 0,
			.max = 3,
			.default_value = 0,
		},
		.nvs_id = 0x0FFF,
	},
	[RB_PARAM_UART_BAUDRATE] = {
		.name = "uart_baudrate",
		.description = "Data UART baud rate",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u32 = {
			.min = 1200,
			.max = 3000000,
			.default_value = 921600,
		},
		.nvs_id = 0x1000,
	},
	[RB_PARAM_UART_RING_SIZE] = {
		.name = "uart_ring_size",
		.description = "Bytes in each data UART ring",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u32 = {
			.min = 1024,
			.max = 32768,
			.default_value = 16384,
		},
		.nvs_id = 0x1001,
	},
	[RB_PARAM_AGGREGATION_TIMEOUT_US] = {
		.name = "aggregation_timeout_us",
		.description = "Partial radio payload aggregation timeout",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u32 = {
			.min = 100,
			.max = 10000,
			.default_value = 2000,
		},
		.nvs_id = 0x1002,
	},
	[RB_PARAM_SYNC_INTERVAL_US] = {
		.name = "sync_interval_us",
		.description = "Wireless sync/discovery interval",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u32 = {
			.min = 10000,
			.max = 1000000,
			.default_value = 30000,
		},
		.nvs_id = 0x1003,
	},
	[RB_PARAM_RESPONSE_SLOT_COUNT] = {
		.name = "response_slot_count",
		.description = "Discovery response slots",
		.type = RB_PARAM_UINT8,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u8 = {
			.min = 1,
			.max = 32,
			.default_value = 4,
		},
		.nvs_id = 0x1004,
	},
	[RB_PARAM_RESPONSE_SLOT_US] = {
		.name = "response_slot_us",
		.description = "Discovery response slot width",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u16 = {
			.min = 100,
			.max = 5000,
			.default_value = 500,
		},
		.nvs_id = 0x1005,
	},
	[RB_PARAM_ASSIGNMENT_WINDOW_US] = {
		.name = "assignment_window_us",
		.description = "Assignment receive window",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u32 = {
			.min = 1000,
			.max = 50000,
			.default_value = 10000,
		},
		.nvs_id = 0x1006,
	},
	[RB_PARAM_LEASE_TIMEOUT_US] = {
		.name = "lease_timeout_us",
		.description = "Lease timeout without valid poll",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u32 = {
			.min = 10000,
			.max = 1000000,
			.default_value = 300000,
		},
		.nvs_id = 0x1007,
	},
	[RB_PARAM_IDLE_POLL_MAX_US] = {
		.name = "idle_poll_max_us",
		.description = "Maximum idle poll interval",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u32 = {
			.min = 1000,
			.max = 10000,
			.default_value = 5000,
		},
		.nvs_id = 0x1008,
	},
	[RB_PARAM_GROUP_ID] = {
		.name = "group_id",
		.description = "Radio group ID (1..0xFFFFFFFE)",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u32 = {
			.min = 1,
			.max = 0xFFFFFFFE,
			.default_value = CONFIG_RADIO_BRIDGE_GROUP_ID,
		},
		.nvs_id = 0x1009,
	},
	[RB_PARAM_GROUP_KEY] = {
		.name = "group_key",
		.description = "Radio group AES-128 key (16 bytes hex)",
		.type = RB_PARAM_BYTES,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.bytes = {
			.length = 16,
			.default_value = default_group_key,
		},
		.nvs_id = 0x100A,
	},
	[RB_PARAM_PPS_PERIOD_US] = {
		.name = "pps_period_us",
		.description = "PPS period in microseconds",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u32 = {
			.min = 100000,
			.max = 10000000,
			.default_value = CONFIG_TIME_SYNC_PPS_PERIOD_US,
		},
		.nvs_id = 0x100B,
	},
	[RB_PARAM_PPS_WIDTH_US] = {
		.name = "pps_width_us",
		.description = "PPS pulse width in microseconds",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u16 = {
			.min = 10,
			.max = 10000,
			.default_value = CONFIG_TIME_SYNC_PPS_WIDTH_US,
		},
		.nvs_id = 0x100C,
	},
	[RB_PARAM_RADIO_DELAY_US] = {
		.name = "radio_delay_us",
		.description = "Calibrated slave radio delay (us)",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u16 = {
			.min = 0,
			.max = 1000,
			.default_value = CONFIG_TIME_SYNC_RADIO_DELAY_US,
		},
		.nvs_id = 0x100D,
	},
	[RB_PARAM_STATUS_INTERVAL_MS] = {
		.name = "status_interval_ms",
		.description = "Status log interval in milliseconds",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u16 = {
			.min = 100,
			.max = 60000,
			.default_value = CONFIG_TIME_SYNC_STATUS_INTERVAL_MS,
		},
		.nvs_id = 0x100E,
	},
	[RB_PARAM_LED_HEARTBEAT] = {
		.name = "led_heartbeat",
		.description = "Enable LED heartbeat blinking",
		.type = RB_PARAM_BOOL,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.boolean = {
			.default_value = IS_ENABLED(CONFIG_TIME_SYNC_LED_HEARTBEAT),
		},
		.nvs_id = 0x100F,
	},
	[RB_PARAM_LED_PERIOD_MS] = {
		.name = "led_period_ms",
		.description = "LED heartbeat period in milliseconds",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_RUNTIME_UPDATE,
		.config.u16 = {
			.min = 100,
			.max = 10000,
#if IS_ENABLED(CONFIG_TIME_SYNC_LED_HEARTBEAT)
			.default_value = CONFIG_TIME_SYNC_LED_HEARTBEAT_PERIOD_MS,
#else
			.default_value = 1000,
#endif
		},
		.nvs_id = 0x1010,
	},
	[RB_PARAM_TIME_UART_BAUDRATE] = {
		.name = "time_uart_baudrate",
		.description = "External time UART baud rate",
		.type = RB_PARAM_UINT32,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u32 = {
			.min = 1200,
			.max = 115200,
			.default_value = CONFIG_TIME_UART_BAUDRATE,
		},
		.nvs_id = 0x1011,
	},
	[RB_PARAM_TIME_SOURCE_MODE] = {
		.name = "time_source_mode",
		.description = "Time source: 0=local, 1=external",
		.type = RB_PARAM_UINT8,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u8 = {
			.min = 0,
			.max = 1,
			.default_value = 0,
		},
		.nvs_id = 0x1012,
	},
	[RB_PARAM_PPS_INPUT_DELAY_US] = {
		.name = "pps_input_delay_us",
		.description = "External PPS input delay compensation (us)",
		.type = RB_PARAM_UINT16,
		.flags = RB_PARAM_FLAG_REBOOT_REQUIRED,
		.config.u16 = {
			.min = 0,
			.max = 1000,
			.default_value = 0,
		},
		.nvs_id = 0x1013,
	},
};

/* Get default value for a parameter */
static uint32_t param_get_default(enum rb_param_id id)
{
	const struct rb_param_descriptor *desc = &rb_param_table[id];

	switch (desc->type) {
	case RB_PARAM_UINT32:
		return desc->config.u32.default_value;
	case RB_PARAM_UINT16:
		return desc->config.u16.default_value;
	case RB_PARAM_UINT8:
		return desc->config.u8.default_value;
	case RB_PARAM_BOOL:
		return desc->config.boolean.default_value ? 1U : 0U;
	default:
		return 0;
	}
}

/* Validate parameter value against range */
static bool param_validate_range(enum rb_param_id id, uint32_t value)
{
	const struct rb_param_descriptor *desc = &rb_param_table[id];

	switch (desc->type) {
	case RB_PARAM_UINT32:
		return value >= desc->config.u32.min && value <= desc->config.u32.max;
	case RB_PARAM_UINT16:
		return value >= desc->config.u16.min && value <= desc->config.u16.max;
	case RB_PARAM_UINT8:
		return value >= desc->config.u8.min && value <= desc->config.u8.max;
	case RB_PARAM_BOOL:
		return value <= 1U;
	default:
		return false;
	}
}

/* Settings subsystem callback for loading parameters.
 *
 * The settings framework already strips the handler's own registered name
 * ("params") off the front of the full key before calling this handler, so
 * `name` arrives as just the remainder -- e.g. "0x0fff", not "params/0x0fff".
 * Re-matching PARAM_NVS_NAMESPACE against that remainder can never succeed. */
static int param_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			      void *cb_arg)
{
	uint16_t nvs_id;
	enum rb_param_id id;
	uint32_t value;
	uint8_t bytes[32];
	ssize_t rc;

	nvs_id = (uint16_t)strtoul(name, NULL, 16);

	/* Find parameter by NVS ID */
	for (id = 0; id < RB_PARAM_COUNT; id++) {
		if (rb_param_table[id].nvs_id == nvs_id) {
			if (rb_param_table[id].type == RB_PARAM_BYTES) {
				/* Read byte array */
				rc = read_cb(cb_arg, bytes, rb_param_table[id].config.bytes.length);
				if (rc == rb_param_table[id].config.bytes.length) {
					memcpy(param_cache[id].data.bytes, bytes,
					       rb_param_table[id].config.bytes.length);
					param_cache[id].is_persisted = true;
					LOG_DBG("Loaded %s (%d bytes, nvs_id 0x%04x)",
						rb_param_table[id].name, (int)rc, nvs_id);
				}
			} else {
				/* Read value from settings */
				rc = read_cb(cb_arg, &value, sizeof(value));
				if (rc == sizeof(value)) {
					if (param_validate_range(id, value)) {
						param_cache[id].data.value = value;
						param_cache[id].is_persisted = true;
						LOG_DBG("Loaded %s = %u (nvs_id 0x%04x)",
							rb_param_table[id].name, value, nvs_id);
					} else {
						LOG_WRN("Ignoring out-of-range persisted %s = %u",
							rb_param_table[id].name, value);
					}
				}
			}
			return 0;
		}
	}

	return 0;
}

static struct settings_handler param_settings = {
	.name = PARAM_NVS_NAMESPACE,
	.h_set = param_settings_set,
};

int rb_param_config_init(void)
{
	int ret;
	enum rb_param_id id;

	if (initialized) {
		return 0;
	}

	/* Parse default group key from Kconfig */
	ret = rb_group_key_parse(CONFIG_RADIO_BRIDGE_GROUP_KEY, default_group_key);
	if (ret != 0) {
		LOG_ERR("Failed to parse CONFIG_RADIO_BRIDGE_GROUP_KEY: %d", ret);
		return ret;
	}

	/* Initialize cache with default values */
	for (id = 0; id < RB_PARAM_COUNT; id++) {
		if (rb_param_table[id].type == RB_PARAM_BYTES) {
			/* Initialize bytes parameters with default or zeros */
			if (rb_param_table[id].config.bytes.default_value) {
				memcpy(param_cache[id].data.bytes,
				       rb_param_table[id].config.bytes.default_value,
				       rb_param_table[id].config.bytes.length);
			} else {
				memset(param_cache[id].data.bytes, 0,
				       rb_param_table[id].config.bytes.length);
			}
		} else {
			param_cache[id].data.value = param_get_default(id);
		}
		param_cache[id].is_persisted = false;
	}

	/* Bind the settings subsystem to its storage backend (NVS on the
	 * "storage" flash partition). Without this, settings_save_one()
	 * always fails with -ENOENT: no backend has been mounted yet. */
	ret = settings_subsys_init();
	if (ret != 0) {
		LOG_ERR("settings_subsys_init failed: %d", ret);
		return ret;
	}

	/* Register settings handler */
	ret = settings_register(&param_settings);
	if (ret != 0) {
		LOG_ERR("settings_register failed: %d", ret);
		return ret;
	}

	/* Load persisted parameters from NVS */
	ret = settings_load_subtree(PARAM_NVS_NAMESPACE);
	if (ret != 0) {
		LOG_ERR("settings_load_subtree failed: %d", ret);
		return ret;
	}

	initialized = true;
	LOG_INF("Parameter config initialized");
	return 0;
}

int rb_param_get_uint32(enum rb_param_id id, uint32_t *value)
{
	if (id >= RB_PARAM_COUNT || value == NULL) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	*value = param_cache[id].data.value;
	return 0;
}

int rb_param_get_bool(enum rb_param_id id, bool *value)
{
	if (id >= RB_PARAM_COUNT || value == NULL) {
		return -EINVAL;
	}

	if (rb_param_table[id].type != RB_PARAM_BOOL) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	*value = (param_cache[id].data.value != 0);
	return 0;
}

int rb_param_get_bytes(enum rb_param_id id, uint8_t *buffer, size_t *length)
{
	if (id >= RB_PARAM_COUNT || buffer == NULL || length == NULL) {
		return -EINVAL;
	}

	if (rb_param_table[id].type != RB_PARAM_BYTES) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	size_t param_len = rb_param_table[id].config.bytes.length;
	if (*length < param_len) {
		return -ENOSPC;
	}

	memcpy(buffer, param_cache[id].data.bytes, param_len);
	*length = param_len;
	return 0;
}

int rb_param_set_uint32(enum rb_param_id id, uint32_t value)
{
	char name[32];
	k_spinlock_key_t key;
	int ret;

	if (id >= RB_PARAM_COUNT) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	/* Validate range */
	if (!param_validate_range(id, value)) {
		LOG_ERR("Value %u out of range for %s", value, rb_param_table[id].name);
		return -EINVAL;
	}

	/* Persist to NVS */
	snprintf(name, sizeof(name), "%s/0x%04x", PARAM_NVS_NAMESPACE,
		 rb_param_table[id].nvs_id);

	key = k_spin_lock(&param_lock);
	ret = settings_save_one(name, &value, sizeof(value));
	if (ret == 0) {
		param_cache[id].data.value = value;
		param_cache[id].is_persisted = true;
	}
	k_spin_unlock(&param_lock, key);

	if (ret != 0) {
		LOG_ERR("settings_save_one failed: %d", ret);
		return -EIO;
	}

	LOG_INF("Set %s = %u", rb_param_table[id].name, value);
	return 0;
}

int rb_param_set_bool(enum rb_param_id id, bool value)
{
	if (id >= RB_PARAM_COUNT) {
		return -EINVAL;
	}

	if (rb_param_table[id].type != RB_PARAM_BOOL) {
		return -EINVAL;
	}

	return rb_param_set_uint32(id, value ? 1U : 0U);
}

int rb_param_set_bytes(enum rb_param_id id, const uint8_t *buffer, size_t length)
{
	char name[32];
	k_spinlock_key_t key;
	int ret;

	if (id >= RB_PARAM_COUNT || buffer == NULL) {
		return -EINVAL;
	}

	if (rb_param_table[id].type != RB_PARAM_BYTES) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	if (length != rb_param_table[id].config.bytes.length) {
		LOG_ERR("Length %zu != expected %zu for %s",
			length, rb_param_table[id].config.bytes.length,
			rb_param_table[id].name);
		return -EINVAL;
	}

	/* Persist to NVS */
	snprintf(name, sizeof(name), "%s/0x%04x", PARAM_NVS_NAMESPACE,
		 rb_param_table[id].nvs_id);

	key = k_spin_lock(&param_lock);
	ret = settings_save_one(name, buffer, length);
	if (ret == 0) {
		memcpy(param_cache[id].data.bytes, buffer, length);
		param_cache[id].is_persisted = true;
	}
	k_spin_unlock(&param_lock, key);

	if (ret != 0) {
		LOG_ERR("settings_save_one failed: %d", ret);
		return -EIO;
	}

	LOG_INF("Set %s (%zu bytes)", rb_param_table[id].name, length);
	return 0;
}

int rb_param_clear(enum rb_param_id id)
{
	char name[32];
	k_spinlock_key_t key;
	int ret;

	if (id >= RB_PARAM_COUNT) {
		return -EINVAL;
	}

	if (!initialized) {
		return -EAGAIN;
	}

	/* Delete from NVS */
	snprintf(name, sizeof(name), "%s/0x%04x", PARAM_NVS_NAMESPACE,
		 rb_param_table[id].nvs_id);

	key = k_spin_lock(&param_lock);
	ret = settings_delete(name);
	if (ret == 0 || ret == -ENOENT) {
		if (rb_param_table[id].type == RB_PARAM_BYTES) {
			if (rb_param_table[id].config.bytes.default_value) {
				memcpy(param_cache[id].data.bytes,
				       rb_param_table[id].config.bytes.default_value,
				       rb_param_table[id].config.bytes.length);
			} else {
				memset(param_cache[id].data.bytes, 0,
				       rb_param_table[id].config.bytes.length);
			}
		} else {
			param_cache[id].data.value = param_get_default(id);
		}
		param_cache[id].is_persisted = false;
		ret = 0;
	}
	k_spin_unlock(&param_lock, key);

	if (ret != 0) {
		LOG_ERR("settings_delete failed: %d", ret);
		return -EIO;
	}

	if (rb_param_table[id].type == RB_PARAM_BYTES) {
		LOG_INF("Cleared %s, using default", rb_param_table[id].name);
	} else {
		LOG_INF("Cleared %s, using default (%u)", rb_param_table[id].name,
			param_cache[id].data.value);
	}
	return 0;
}

int rb_param_reset_all(void)
{
	enum rb_param_id id;
	int ret;
	int last_error = 0;

	if (!initialized) {
		return -EAGAIN;
	}

	for (id = 0; id < RB_PARAM_COUNT; id++) {
		if (param_cache[id].is_persisted) {
			ret = rb_param_clear(id);
			if (ret != 0) {
				last_error = ret;
			}
		}
	}

	if (last_error == 0) {
		LOG_INF("All parameters reset to defaults");
	}

	return last_error;
}

bool rb_param_requires_reboot(enum rb_param_id id)
{
	if (id >= RB_PARAM_COUNT) {
		return false;
	}

	return (rb_param_table[id].flags & RB_PARAM_FLAG_REBOOT_REQUIRED) != 0;
}

const struct rb_param_descriptor *rb_param_get_descriptor(enum rb_param_id id)
{
	if (id >= RB_PARAM_COUNT) {
		return NULL;
	}

	return &rb_param_table[id];
}

bool rb_param_is_persisted(enum rb_param_id id)
{
	if (id >= RB_PARAM_COUNT) {
		return false;
	}

	if (!initialized) {
		return false;
	}

	return param_cache[id].is_persisted;
}
