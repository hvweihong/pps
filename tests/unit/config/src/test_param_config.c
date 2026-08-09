/*
 * Unit tests for parameter configuration
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <stdlib.h>
#include "param_config.h"

void rb_param_config_test_reset(void);

/* Mock settings subsystem */
static struct {
	uint8_t values[RB_PARAM_COUNT][32];
	size_t lengths[RB_PARAM_COUNT];
	bool is_set[RB_PARAM_COUNT];
} mock_nvs;

static bool nvs_initialized;
static int mock_init_error;
static int mock_load_error;
static struct settings_handler *mock_handler;

struct mock_read_context {
	enum rb_param_id id;
};

static ssize_t mock_read_cb(void *cb_arg, void *data, size_t len)
{
	struct mock_read_context *context = cb_arg;
	size_t stored = mock_nvs.lengths[context->id];

	if (len < stored) {
		return -ENOSPC;
	}
	memcpy(data, mock_nvs.values[context->id], stored);
	return (ssize_t)stored;
}

static uint32_t mock_uint32(enum rb_param_id id)
{
	uint32_t value = 0u;

	memcpy(&value, mock_nvs.values[id], sizeof(value));
	return value;
}

int settings_subsys_init(void)
{
	nvs_initialized = true;
	return mock_init_error;
}

int settings_register(struct settings_handler *handler)
{
	mock_handler = handler;
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
	char name[16];

	ARG_UNUSED(subtree);
	if (mock_load_error != 0) {
		return mock_load_error;
	}
	for (enum rb_param_id id = 0; id < RB_PARAM_COUNT; id++) {
		struct mock_read_context context = {.id = id};

		if (!mock_nvs.is_set[id]) {
			continue;
		}
		snprintk(name, sizeof(name), "0x%04x", rb_param_table[id].nvs_id);
		zassert_not_null(mock_handler);
		zassert_ok(mock_handler->h_set(name, mock_nvs.lengths[id],
					       mock_read_cb, &context));
	}
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
			if (val_len <= sizeof(mock_nvs.values[id])) {
				memcpy(mock_nvs.values[id], value, val_len);
				mock_nvs.lengths[id] = val_len;
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
	return NULL;
}

static void param_config_before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&mock_nvs, 0, sizeof(mock_nvs));
	nvs_initialized = false;
	mock_init_error = 0;
	mock_load_error = 0;
	mock_handler = NULL;
	rb_param_config_test_reset();
}

ZTEST_SUITE(param_config, NULL, param_config_setup, param_config_before, NULL, NULL);

ZTEST(param_config, test_only_deployment_parameters_are_persistent)
{
	static const char *const expected_names[] = {
		"role_id", "group_id", "group_key", "uart_baudrate",
		"time_source_mode", "time_uart_baudrate",
		"pps_input_delay_us", "radio_delay_us",
	};

	zassert_equal(RB_PARAM_COUNT, ARRAY_SIZE(expected_names));
	for (size_t i = 0; i < ARRAY_SIZE(expected_names); i++) {
		zassert_equal(strcmp(rb_param_table[i].name, expected_names[i]), 0);
		zassert_equal(rb_param_table[i].nvs_id, 0x1000u + i);
		zassert_true(rb_param_requires_reboot((enum rb_param_id)i));
	}
}

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
	zassert_equal(value, 921600, "Should return compiled UART default");

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
	zassert_equal(mock_uint32(RB_PARAM_UART_BAUDRATE), 115200,
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
	rb_param_set_uint32(RB_PARAM_GROUP_ID, 2);
	rb_param_set_uint32(RB_PARAM_RADIO_DELAY_US, 10);

	zassert_true(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		     "Should be persisted");
	zassert_true(rb_param_is_persisted(RB_PARAM_GROUP_ID),
		     "Should be persisted");
	zassert_true(rb_param_is_persisted(RB_PARAM_RADIO_DELAY_US),
		     "Should be persisted");

	/* Reset all */
	ret = rb_param_reset_all();
	zassert_equal(ret, 0, "Reset should succeed");

	/* All should be cleared */
	zassert_false(rb_param_is_persisted(RB_PARAM_UART_BAUDRATE),
		      "Should not be persisted");
	zassert_false(rb_param_is_persisted(RB_PARAM_GROUP_ID),
		      "Should not be persisted");
	zassert_false(rb_param_is_persisted(RB_PARAM_RADIO_DELAY_US),
		      "Should not be persisted");
}

ZTEST(param_config, test_missing_backend_keeps_defaults)
{
	uint32_t value;

	mock_init_error = -ENODEV;
	zassert_ok(rb_param_config_init());
	zassert_false(rb_param_persistence_available());
	zassert_ok(rb_param_get_uint32(RB_PARAM_GROUP_ID, &value));
	zassert_equal(value, 1u);
	zassert_equal(rb_param_set_uint32(RB_PARAM_GROUP_ID, 2u), -ENOTSUP);
}

ZTEST(param_config, test_load_failure_keeps_defaults)
{
	uint32_t persisted = 2u;
	uint32_t value;

	memcpy(mock_nvs.values[RB_PARAM_GROUP_ID], &persisted, sizeof(persisted));
	mock_nvs.lengths[RB_PARAM_GROUP_ID] = sizeof(persisted);
	mock_nvs.is_set[RB_PARAM_GROUP_ID] = true;
	mock_load_error = -EIO;
	zassert_ok(rb_param_config_init());
	zassert_false(rb_param_persistence_available());
	zassert_ok(rb_param_get_uint32(RB_PARAM_GROUP_ID, &value));
	zassert_equal(value, 1u);
}

ZTEST(param_config, test_group_key_set_get_round_trip)
{
	const uint8_t expected[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	uint8_t actual[16];
	size_t actual_len = sizeof(actual);

	zassert_ok(rb_param_config_init());
	zassert_ok(rb_param_set_bytes(RB_PARAM_GROUP_KEY, expected, sizeof(expected)));
	zassert_true(mock_nvs.is_set[RB_PARAM_GROUP_KEY]);
	zassert_equal(mock_nvs.lengths[RB_PARAM_GROUP_KEY], sizeof(expected));
	zassert_ok(rb_param_get_bytes(RB_PARAM_GROUP_KEY, actual, &actual_len));
	zassert_equal(actual_len, sizeof(expected));
	zassert_mem_equal(actual, expected, sizeof(expected));
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
	zassert_equal(desc->nvs_id, 0x1003, "NVS ID should match");

	/* Invalid ID */
	desc = rb_param_get_descriptor(RB_PARAM_COUNT);
	zassert_is_null(desc, "Should return NULL for invalid ID");
}

ZTEST(param_config, test_u8_parameter)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* role_id is uint8 */
	ret = rb_param_set_uint32(RB_PARAM_ROLE_ID, 3);
	zassert_equal(ret, 0, "Set should succeed");

	ret = rb_param_get_uint32(RB_PARAM_ROLE_ID, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 3, "Should return correct value");

	ret = rb_param_set_uint32(RB_PARAM_ROLE_ID, 4);
	zassert_equal(ret, -EINVAL, "Should reject out-of-range");
}

ZTEST(param_config, test_u16_parameter)
{
	uint32_t value;
	int ret;

	rb_param_config_init();

	/* radio_delay_us is uint16 */
	ret = rb_param_set_uint32(RB_PARAM_RADIO_DELAY_US, 1000);
	zassert_equal(ret, 0, "Set should succeed");

	ret = rb_param_get_uint32(RB_PARAM_RADIO_DELAY_US, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 1000, "Should return correct value");

	/* Test range */
	ret = rb_param_set_uint32(RB_PARAM_RADIO_DELAY_US, 1001);
	zassert_equal(ret, -EINVAL, "Should reject above max");
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
