/*
 * Unit tests for parameter configuration
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include "param_config.h"

/* Mock settings subsystem */
static struct {
	uint32_t values[RB_PARAM_COUNT];
	bool is_set[RB_PARAM_COUNT];
} mock_nvs;

static bool nvs_initialized;

int settings_subsys_init(void)
{
	nvs_initialized = true;
	return 0;
}

int settings_register(struct settings_handler *handler)
{
	return 0;
}

int settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t len = strlen(key);
	if (strncmp(name, key, len) == 0) {
		if (name[len] == '/') {
			*next = &name[len + 1];
			return 1;
		}
	}
	return 0;
}

int settings_load_subtree(const char *subtree)
{
	return 0;
}

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	/* Parse NVS ID from name (format: "params/0x1000") */
	const char *id_str = strrchr(name, '/');
	uint16_t nvs_id;
	enum rb_param_id id;

	if (id_str == NULL) {
		return -EINVAL;
	}

	nvs_id = (uint16_t)strtoul(id_str + 1, NULL, 16);

	/* Find parameter by NVS ID */
	for (id = 0; id < RB_PARAM_COUNT; id++) {
		if (rb_param_table[id].nvs_id == nvs_id) {
			if (val_len == sizeof(uint32_t)) {
				memcpy(&mock_nvs.values[id], value, sizeof(uint32_t));
				mock_nvs.is_set[id] = true;
				return 0;
			}
			return -EINVAL;
		}
	}

	return -ENOENT;
}

int settings_delete(const char *name)
{
	const char *id_str = strrchr(name, '/');
	uint16_t nvs_id;
	enum rb_param_id id;

	if (id_str == NULL) {
		return -EINVAL;
	}

	nvs_id = (uint16_t)strtoul(id_str + 1, NULL, 16);

	for (id = 0; id < RB_PARAM_COUNT; id++) {
		if (rb_param_table[id].nvs_id == nvs_id) {
			mock_nvs.is_set[id] = false;
			return 0;
		}
	}

	return -ENOENT;
}

/* Test setup/teardown */
static void *param_config_setup(void)
{
	memset(&mock_nvs, 0, sizeof(mock_nvs));
	nvs_initialized = false;
	return NULL;
}

ZTEST_SUITE(param_config, NULL, param_config_setup, NULL, NULL, NULL);

ZTEST(param_config, test_init)
{
	/* Reset nvs_initialized and call settings_subsys_init explicitly */
	nvs_initialized = false;
	settings_subsys_init();

	int ret = rb_param_config_init();

	zassert_equal(ret, 0, "Init should succeed");
	zassert_true(nvs_initialized, "NVS should be initialized");
}

ZTEST(param_config, test_get_default_values)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* UART baudrate should match default */
	ret = rb_param_get_uint32(RB_PARAM_UART_BAUDRATE, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 921600, "Should return hardcoded default");

	/* Aggregation timeout */
	ret = rb_param_get_uint32(RB_PARAM_AGGREGATION_TIMEOUT_US, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 2000, "Should return hardcoded default");

	zassert_ok(rb_param_get_uint32(RB_PARAM_TIME_SOURCE_MODE, &value));
	zassert_equal(value, 0, "Local time source is the reboot default");
	zassert_ok(rb_param_get_uint32(RB_PARAM_PPS_INPUT_DELAY_US, &value));
	zassert_equal(value, 0, "PPS input delay defaults to zero");
}

ZTEST(param_config, test_external_time_parameters_are_reboot_effective_and_bounded)
{
	rb_param_config_init();
	zassert_true(rb_param_requires_reboot(RB_PARAM_TIME_SOURCE_MODE));
	zassert_true(rb_param_requires_reboot(RB_PARAM_PPS_INPUT_DELAY_US));
	zassert_equal(rb_param_set_uint32(RB_PARAM_TIME_SOURCE_MODE, 2), -EINVAL);
	zassert_equal(rb_param_set_uint32(RB_PARAM_PPS_INPUT_DELAY_US, 1001), -EINVAL);
	zassert_ok(rb_param_set_uint32(RB_PARAM_TIME_SOURCE_MODE, 1));
	zassert_ok(rb_param_set_uint32(RB_PARAM_PPS_INPUT_DELAY_US, 1000));
	zassert_ok(rb_param_clear(RB_PARAM_TIME_SOURCE_MODE));
	zassert_ok(rb_param_clear(RB_PARAM_PPS_INPUT_DELAY_US));
}

ZTEST(param_config, test_set_and_get)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* Set UART baudrate to 115200 */
	ret = rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 115200);
	zassert_equal(ret, 0, "Set should succeed");
	zassert_true(mock_nvs.is_set[RB_PARAM_UART_BAUDRATE],
		     "Value should be persisted");
	zassert_equal(mock_nvs.values[RB_PARAM_UART_BAUDRATE], 115200,
		      "Persisted value should match");

	/* Read back */
	ret = rb_param_get_uint32(RB_PARAM_UART_BAUDRATE, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 115200, "Should return persisted value");
}

ZTEST(param_config, test_range_validation)
{
	int ret;

	rb_param_config_init();

	/* Try to set baudrate below minimum */
	ret = rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 1000);
	zassert_equal(ret, -EINVAL, "Should reject out-of-range value");
	zassert_false(mock_nvs.is_set[RB_PARAM_UART_BAUDRATE],
		      "Should not persist invalid value");

	/* Try to set baudrate above maximum */
	ret = rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 4000000);
	zassert_equal(ret, -EINVAL, "Should reject out-of-range value");

	/* Valid range should work */
	ret = rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 921600);
	zassert_equal(ret, 0, "Should accept valid value");
}

ZTEST(param_config, test_clear)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* Set a value */
	rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 230400);
	zassert_true(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		     "Should be persisted");

	/* Clear it */
	ret = rb_param_clear(RB_PARAM_UART_BAUDRATE);
	zassert_equal(ret, 0, "Clear should succeed");
	zassert_false(mock_nvs.is_set[RB_PARAM_UART_BAUDRATE],
		      "Should not be persisted");
	zassert_false(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		      "Should report not persisted");

	/* Should return default */
	ret = rb_param_get_uint32(RB_PARAM_UART_BAUDRATE, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 921600, "Should return default after clear");
}

ZTEST(param_config, test_reset_all)
{
	int ret;

	rb_param_config_init();

	/* Set multiple parameters */
	rb_param_set_uint32(RB_PARAM_UART_BAUDRATE, 115200);
	rb_param_set_uint32(RB_PARAM_AGGREGATION_TIMEOUT_US, 5000);
	rb_param_set_uint32(RB_PARAM_SYNC_INTERVAL_US, 50000);

	zassert_true(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		     "Should be persisted");
	zassert_true(rb_param_is_persisted(RB_PARAM_AGGREGATION_TIMEOUT_US),
		     "Should be persisted");
	zassert_true(rb_param_is_persisted(RB_PARAM_SYNC_INTERVAL_US),
		     "Should be persisted");

	/* Reset all */
	ret = rb_param_reset_all();
	zassert_equal(ret, 0, "Reset should succeed");

	/* All should be cleared */
	zassert_false(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		      "Should not be persisted");
	zassert_false(rb_param_is_persisted(RB_PARAM_AGGREGATION_TIMEOUT_US),
		      "Should not be persisted");
	zassert_false(rb_param_is_persisted(RB_PARAM_SYNC_INTERVAL_US),
		      "Should not be persisted");
}

ZTEST(param_config, test_requires_reboot)
{
	rb_param_config_init();

	/* UART parameters require reboot */
	zassert_true(rb_param_requires_reboot(RB_PARAM_UART_BAUDRATE),
		     "UART baudrate requires reboot");
	zassert_true(rb_param_requires_reboot(RB_PARAM_UART_RING_SIZE),
		     "UART ring size requires reboot");
	zassert_true(rb_param_requires_reboot(RB_PARAM_TIME_UART_BAUDRATE),
		     "time UART baudrate requires reboot");

	/* Runtime parameters don't */
	zassert_false(rb_param_requires_reboot(RB_PARAM_AGGREGATION_TIMEOUT_US),
		      "Aggregation timeout is runtime");
	zassert_false(rb_param_requires_reboot(RB_PARAM_SYNC_INTERVAL_US),
		      "Sync interval is runtime");
}

ZTEST(param_config, test_time_uart_baudrate_range)
{
	uint32_t value;

	rb_param_config_init();
	zassert_equal(rb_param_get_uint32(RB_PARAM_TIME_UART_BAUDRATE, &value), 0,
		      "get time UART baudrate");
	zassert_equal(value, 9600, "default time UART baudrate");
	zassert_equal(rb_param_set_uint32(RB_PARAM_TIME_UART_BAUDRATE, 1200), 0,
		      "minimum time UART baudrate");
	zassert_equal(rb_param_set_uint32(RB_PARAM_TIME_UART_BAUDRATE, 115200), 0,
		      "maximum time UART baudrate");
	zassert_equal(rb_param_set_uint32(RB_PARAM_TIME_UART_BAUDRATE, 1199), -EINVAL,
		      "reject below minimum");
	zassert_equal(rb_param_set_uint32(RB_PARAM_TIME_UART_BAUDRATE, 115201), -EINVAL,
		      "reject above maximum");
}

ZTEST(param_config, test_get_descriptor)
{
	const struct rb_param_descriptor *desc;

	rb_param_config_init();

	desc = rb_param_get_descriptor(RB_PARAM_UART_BAUDRATE);
	zassert_not_null(desc, "Should return descriptor");
	zassert_equal(strcmp(desc->name, "uart_baudrate"), 0,
		      "Name should match");
	zassert_equal(desc->type, RB_PARAM_UINT32, "Type should be uint32");
	zassert_equal(desc->nvs_id, 0x1000, "NVS ID should match");

	/* Invalid ID */
	desc = rb_param_get_descriptor(RB_PARAM_COUNT);
	zassert_is_null(desc, "Should return NULL for invalid ID");
}

ZTEST(param_config, test_u8_parameter)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* RESPONSE_SLOT_COUNT is uint8 */
	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_COUNT, 8);
	zassert_equal(ret, 0, "Set should succeed");

	ret = rb_param_get_uint32(RB_PARAM_RESPONSE_SLOT_COUNT, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 8, "Should return correct value");

	/* Test range - max is 32 */
	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_COUNT, 33);
	zassert_equal(ret, -EINVAL, "Should reject out-of-range");

	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_COUNT, 0);
	zassert_equal(ret, -EINVAL, "Should reject below min");
}

ZTEST(param_config, test_u16_parameter)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* RESPONSE_SLOT_US is uint16 */
	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_US, 1000);
	zassert_equal(ret, 0, "Set should succeed");

	ret = rb_param_get_uint32(RB_PARAM_RESPONSE_SLOT_US, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 1000, "Should return correct value");

	/* Test range */
	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_US, 6000);
	zassert_equal(ret, -EINVAL, "Should reject above max");

	ret = rb_param_set_uint32(RB_PARAM_RESPONSE_SLOT_US, 50);
	zassert_equal(ret, -EINVAL, "Should reject below min");
}

ZTEST(param_config, test_invalid_param_id)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	ret = rb_param_get_uint32(RB_PARAM_COUNT, &value);
	zassert_equal(ret, -EINVAL, "Should reject invalid ID");

	ret = rb_param_set_uint32(RB_PARAM_COUNT, 100);
	zassert_equal(ret, -EINVAL, "Should reject invalid ID");

	ret = rb_param_clear(RB_PARAM_COUNT);
	zassert_equal(ret, -EINVAL, "Should reject invalid ID");
}
