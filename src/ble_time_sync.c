#include "ble_time_sync.h"

#include "app_config.h"
#include "ble_time_sync_client.h"
#include "ble_time_sync_service.h"
#include "ble_time_sync_uuids.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(ble_time_sync, LOG_LEVEL_INF);

K_MSGQ_DEFINE(ble_cmd_msgq, sizeof(struct ble_time_sync_command), 4, 4);

static atomic_t connected_count;
static atomic_t initialized;
static atomic_t bt_ready;
static atomic_t started;
static atomic_t start_attempts;
static atomic_t last_error;

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
static atomic_t ble_advertising;

static const struct bt_data slave_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS,
		      (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_TIME_SYNC_SERVICE_VAL),
};
static char slave_name[CONFIG_BT_DEVICE_NAME_MAX];
static struct bt_data slave_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, slave_name, 0),
};
static const struct bt_le_adv_param slave_adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
			     BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static int slave_advertising_start(void)
{
	int ret = bt_le_adv_start(&slave_adv_param, slave_ad,
				  ARRAY_SIZE(slave_ad), slave_sd,
				  ARRAY_SIZE(slave_sd));

	if (ret == 0 || ret == -EALREADY) {
		atomic_set(&ble_advertising, 1);
	} else {
		atomic_set(&last_error, ret);
	}

	return ret;
}

static void command_received(const char *cmd, size_t len)
{
	struct ble_time_sync_command queued = {0};
	size_t copy_len;

	if (cmd == NULL || len == 0u) {
		return;
	}

	copy_len = MIN(len, sizeof(queued.text) - 1u);
	memcpy(queued.text, cmd, copy_len);

	if (k_msgq_put(&ble_cmd_msgq, &queued, K_NO_WAIT) != 0) {
		LOG_WRN("BLE command queue full");
	}
}
#endif

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err != 0) {
		LOG_WRN("BLE connect failed: %u", err);
#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
		if (atomic_get(&started) == 0) {
			return;
		}

		int ret = slave_advertising_start();

		if (ret != 0 && ret != -EALREADY) {
			LOG_WRN("BLE advertising restart failed: %d", ret);
		}
#endif
		return;
	}

	atomic_inc(&connected_count);
#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	atomic_set(&ble_advertising, 0);
#endif
	LOG_INF("BLE connected count=%u", (uint32_t)atomic_get(&connected_count));
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	if (atomic_get(&connected_count) > 0) {
		atomic_dec(&connected_count);
	}

	LOG_INF("BLE disconnected reason=%u count=%u", reason,
		(uint32_t)atomic_get(&connected_count));

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	if (atomic_get(&started) == 0) {
		return;
	}

	int ret = slave_advertising_start();

	if (ret != 0 && ret != -EALREADY) {
		LOG_WRN("BLE advertising restart failed: %d", ret);
	}
#endif
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

bool ble_time_sync_connected(void)
{
	return atomic_get(&connected_count) > 0;
}

uint8_t ble_time_sync_connection_count(void)
{
	return (uint8_t)atomic_get(&connected_count);
}

int ble_time_sync_get_command(struct ble_time_sync_command *cmd)
{
	if (cmd == NULL) {
		return -EINVAL;
	}

	return k_msgq_get(&ble_cmd_msgq, cmd, K_NO_WAIT);
}

int ble_time_sync_init(void)
{
	if (atomic_get(&initialized) != 0) {
		return 0;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	snprintk(slave_name, sizeof(slave_name), "%s-SLAVE",
		 TIME_SYNC_BLE_DEVICE_NAME_PREFIX_VALUE);
	slave_sd[0].data_len = strlen(slave_name);

	int ret = ble_time_sync_service_init(command_received);
	if (ret != 0) {
		return ret;
	}
#endif

	atomic_set(&initialized, 1);
	return 0;
}

static int ble_time_sync_enable_stack(void)
{
	int ret;

	if (atomic_get(&bt_ready) != 0) {
		return 0;
	}

	ret = ble_time_sync_init();
	if (ret != 0) {
		return ret;
	}

	ret = bt_enable(NULL);
	if (ret != 0) {
		return ret;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	(void)bt_set_name(slave_name);
#elif defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	char name[CONFIG_BT_DEVICE_NAME_MAX];

	snprintk(name, sizeof(name), "%s-MASTER",
		 TIME_SYNC_BLE_DEVICE_NAME_PREFIX_VALUE);
	(void)bt_set_name(name);
#endif

	atomic_set(&bt_ready, 1);
	return 0;
}

int ble_time_sync_start(void)
{
	int ret;

	atomic_inc(&start_attempts);
	ret = ble_time_sync_enable_stack();
	if (ret != 0) {
		atomic_set(&last_error, ret);
		return ret;
	}

	if (atomic_cas(&started, 0, 1) == false) {
		return 0;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	ret = slave_advertising_start();
	if (ret == 0 || ret == -EALREADY) {
		atomic_set(&last_error, 0);
		LOG_INF("BLE advertising as %s", slave_name);
		return 0;
	}
#elif defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	ret = ble_time_sync_client_start();
	if (ret == 0) {
		atomic_set(&last_error, 0);
		LOG_INF("BLE central started");
		return 0;
	}
#else
	ret = -ENOTSUP;
#endif

	atomic_set(&started, 0);
	atomic_set(&last_error, ret);
	return ret;
}

bool ble_time_sync_started(void)
{
	return atomic_get(&started) != 0;
}

void ble_time_sync_get_snapshot(struct ble_time_sync_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}

	snapshot->initialized = atomic_get(&initialized) != 0;
	snapshot->bt_ready = atomic_get(&bt_ready) != 0;
	snapshot->started = atomic_get(&started) != 0;
	snapshot->connected = ble_time_sync_connected();
	snapshot->connection_count = ble_time_sync_connection_count();
	snapshot->peer_count = 0u;
	snapshot->start_attempts = (uint32_t)atomic_get(&start_attempts);
	snapshot->last_error = (int)atomic_get(&last_error);

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	snapshot->scanning = ble_time_sync_client_scanning();
	snapshot->peer = ble_time_sync_client_has_peer();
	snapshot->notify_enabled = ble_time_sync_client_notify_subscribed();
	snapshot->peer_count = ble_time_sync_client_peer_count();
	struct ble_time_sync_client_scan_stats scan_stats;

	ble_time_sync_client_get_scan_stats(&scan_stats);
	snapshot->scan_seen = scan_stats.scan_seen;
	snapshot->scan_match = scan_stats.scan_match;
	snapshot->scan_reject_type = scan_stats.scan_reject_type;
	snapshot->scan_reject_filter = scan_stats.scan_reject_filter;
	snapshot->connect_attempts = scan_stats.connect_attempts;
	snapshot->connect_failures = scan_stats.connect_failures;
	snapshot->gatt_tx_found = scan_stats.gatt_tx_found;
	snapshot->gatt_tx_ccc_found = scan_stats.gatt_tx_ccc_found;
	snapshot->gatt_subscribe_attempts =
		scan_stats.gatt_subscribe_attempts;
	snapshot->gatt_subscribe_failures =
		scan_stats.gatt_subscribe_failures;
	snapshot->gatt_notify_subscribed =
		scan_stats.gatt_notify_subscribed;
	snapshot->gatt_rx_found = scan_stats.gatt_rx_found;
	snapshot->gatt_write_attempts = scan_stats.gatt_write_attempts;
	snapshot->gatt_write_failures = scan_stats.gatt_write_failures;
	snapshot->gatt_write_successes = scan_stats.gatt_write_successes;
#else
	snapshot->scanning = false;
	snapshot->peer = false;
	snapshot->scan_seen = 0;
	snapshot->scan_match = 0;
	snapshot->scan_reject_type = 0;
	snapshot->scan_reject_filter = 0;
	snapshot->connect_attempts = 0;
	snapshot->connect_failures = 0;
	snapshot->gatt_tx_found = 0;
	snapshot->gatt_tx_ccc_found = 0;
	snapshot->gatt_subscribe_attempts = 0;
	snapshot->gatt_subscribe_failures = 0;
	snapshot->gatt_notify_subscribed = 0;
	snapshot->gatt_rx_found = 0;
	snapshot->gatt_write_attempts = 0;
	snapshot->gatt_write_failures = 0;
	snapshot->gatt_write_successes = 0;
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	snapshot->advertising = atomic_get(&ble_advertising) != 0;
	snapshot->notify_enabled = ble_time_sync_service_notify_enabled();
	snapshot->peer_count = snapshot->connection_count;
#else
	snapshot->advertising = false;
#endif
}
