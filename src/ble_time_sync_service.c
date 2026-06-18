#include "ble_time_sync_service.h"

#include "ble_time_sync.h"
#include "ble_time_sync_uuids.h"
#include "runtime_config.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(ble_time_sync_service, LOG_LEVEL_INF);

static ble_time_sync_command_handler_t command_handler;
static char status_buf[128];
static atomic_t notify_enabled;

static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	if (value == BT_GATT_CCC_NOTIFY) {
		atomic_set(&notify_enabled, 1);
		LOG_INF("BLE TX notify enabled");
	} else {
		atomic_set(&notify_enabled, 0);
		LOG_INF("BLE TX notify disabled");
	}
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	const char *status = ble_time_sync_service_status();

	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, status,
				 strlen(status));
}

static ssize_t command_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	char cmd[sizeof(struct ble_time_sync_command)];
	size_t copy_len;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0u) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	copy_len = MIN((size_t)len, sizeof(cmd) - 1u);
	memcpy(cmd, buf, copy_len);
	cmd[copy_len] = '\0';
	LOG_INF("BLE RX command: %s", cmd);

	if (command_handler != NULL) {
		command_handler(cmd, copy_len);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(time_sync_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_TIME_SYNC_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_TIME_SYNC_RX,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, command_write, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_TIME_SYNC_TX,
			       BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_TIME_SYNC_STATUS,
			       BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       status_read, NULL, NULL));

int ble_time_sync_service_init(ble_time_sync_command_handler_t handler)
{
	command_handler = handler;
	return 0;
}

int ble_time_sync_service_notify(const char *msg)
{
	int ret;

	if (msg == NULL) {
		return -EINVAL;
	}

	if (atomic_get(&notify_enabled) == 0) {
		LOG_WRN("BLE notify skipped before subscription: %s", msg);
		return -EAGAIN;
	}

	ret = bt_gatt_notify(NULL, &time_sync_svc.attrs[4], msg, strlen(msg));
	if (ret != 0) {
		LOG_WRN("BLE notify failed: %d msg=%s", ret, msg);
	} else {
		LOG_INF("BLE notify queued: %s", msg);
	}

	return ret;
}

bool ble_time_sync_service_notify_enabled(void)
{
	return atomic_get(&notify_enabled) != 0;
}

const char *ble_time_sync_service_status(void)
{
	struct time_sync_runtime_config cfg = runtime_config_get();

	snprintk(status_buf, sizeof(status_buf),
		 "role=slave conn=%u network=0x%08x channel=%u interval=%u",
		 ble_time_sync_connection_count(), cfg.network_id, cfg.rf_channel,
		 cfg.sync_interval_us);

	return status_buf;
}
