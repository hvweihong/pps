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
	zassert_equal(value, 115200, "Should return compiled UART default");

	/* Aggregation timeout */
	ret = rb_param_get_uint32(RB_PARAM_AGGREGATION_TIMEOUT_US, &value);
	zassert_equal(ret, 0, "Get should succeed");
	zassert_equal(value, 1000, "Should return compiled aggregation default");

	ret = rb_param_get_uint32(RB_PARAM_PPS_WIDTH_US, &value);
	zassert_ok(ret);
	zassert_equal(value, 100000, "PPS pulse width defaults to 100 ms");

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
	zassert_equal(value, 115200, "Should return default after clear");
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

	zassert_true(rb_param_requires_reboot(RB_PARAM_AGGREGATION_TIMEOUT_US));
	zassert_true(rb_param_requires_reboot(RB_PARAM_SYNC_INTERVAL_US));
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

ZTEST(param_config, test_communication_parameters_require_reboot)
{
	static const enum rb_param_id reboot_parameters[] = {
		RB_PARAM_ROLE_ID,
		RB_PARAM_UART_BAUDRATE,
		RB_PARAM_UART_RING_SIZE,
		RB_PARAM_AGGREGATION_TIMEOUT_US,
		RB_PARAM_SYNC_INTERVAL_US,
		RB_PARAM_RESPONSE_SLOT_COUNT,
		RB_PARAM_RESPONSE_SLOT_US,
		RB_PARAM_ASSIGNMENT_WINDOW_US,
		RB_PARAM_LEASE_TIMEOUT_US,
		RB_PARAM_IDLE_POLL_MAX_US,
		RB_PARAM_GROUP_ID,
		RB_PARAM_GROUP_KEY,
		RB_PARAM_PPS_PERIOD_US,
		RB_PARAM_PPS_WIDTH_US,
		RB_PARAM_RADIO_DELAY_US,
		RB_PARAM_TIME_UART_BAUDRATE,
		RB_PARAM_TIME_SOURCE_MODE,
		RB_PARAM_PPS_INPUT_DELAY_US,
	};

	zassert_ok(rb_param_config_init());
	for (size_t i = 0u; i < ARRAY_SIZE(reboot_parameters); i++) {
		zassert_true(rb_param_requires_reboot(reboot_parameters[i]),
			     "parameter %u must require reboot", reboot_parameters[i]);
	}
}

ZTEST(param_config, test_only_status_and_led_are_runtime)
{
	zassert_ok(rb_param_config_init());
	for (enum rb_param_id id = 0; id < RB_PARAM_COUNT; id++) {
		const struct rb_param_descriptor *descriptor = rb_param_get_descriptor(id);
		bool expected_runtime = id == RB_PARAM_STATUS_INTERVAL_MS ||
			id == RB_PARAM_LED_HEARTBEAT || id == RB_PARAM_LED_PERIOD_MS;

		zassert_equal((descriptor->flags & RB_PARAM_FLAG_RUNTIME_UPDATE) != 0u,
			      expected_runtime, "unexpected runtime flag for %u", id);
	}
}

ZTEST(param_config, test_fixed_parameters_do_not_advertise_unsupported_values)
{
	const struct rb_param_descriptor *ring;
	const struct rb_param_descriptor *period;

	zassert_ok(rb_param_config_init());
	ring = rb_param_get_descriptor(RB_PARAM_UART_RING_SIZE);
	period = rb_param_get_descriptor(RB_PARAM_PPS_PERIOD_US);
	zassert_equal(ring->config.u32.min, CONFIG_RADIO_BRIDGE_UART_RING_SIZE);
	zassert_equal(ring->config.u32.max, CONFIG_RADIO_BRIDGE_UART_RING_SIZE);
	zassert_equal(period->config.u32.min, 1000000u);
	zassert_equal(period->config.u32.max, 1000000u);
	zassert_equal(rb_param_set_uint32(RB_PARAM_UART_RING_SIZE, 8192u), -EINVAL);
	zassert_equal(rb_param_set_uint32(RB_PARAM_PPS_PERIOD_US, 500000u), -EINVAL);
}

ZTEST(param_config, test_status_interval_stays_below_watchdog_timeout)
{
	const struct rb_param_descriptor *status;

	zassert_ok(rb_param_config_init());
	status = rb_param_get_descriptor(RB_PARAM_STATUS_INTERVAL_MS);
	zassert_equal(status->config.u16.max, 10000u);
	zassert_ok(rb_param_set_uint32(RB_PARAM_STATUS_INTERVAL_MS, 10000u));
	zassert_equal(rb_param_set_uint32(RB_PARAM_STATUS_INTERVAL_MS, 10001u),
		      -EINVAL);
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

ZTEST(param_config, test_external_time_parameter_nvs_ids_are_stable)
{
	const struct rb_param_descriptor *time_source;
	const struct rb_param_descriptor *time_uart;
	const struct rb_param_descriptor *pps_delay;

	zassert_ok(rb_param_config_init());
	time_source = rb_param_get_descriptor(RB_PARAM_TIME_SOURCE_MODE);
	time_uart = rb_param_get_descriptor(RB_PARAM_TIME_UART_BAUDRATE);
	pps_delay = rb_param_get_descriptor(RB_PARAM_PPS_INPUT_DELAY_US);
	zassert_equal(time_source->nvs_id, 0x1011);
	zassert_equal(time_uart->nvs_id, 0x1012);
	zassert_equal(pps_delay->nvs_id, 0x1013);
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
