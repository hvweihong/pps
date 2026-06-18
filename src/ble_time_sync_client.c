#include "ble_time_sync_client.h"

#include "app_config.h"
#include "ble_time_sync_uuids.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(ble_time_sync_client, LOG_LEVEL_INF);

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
#define SLAVE_NAME_SUFFIX "-SLAVE"
#define GATT_WRITE_RETRY_DELAY_MS 100
#define GATT_WRITE_NEXT_DELAY_MS 10

enum gatt_config_step {
	GATT_CONFIG_IDLE,
	GATT_CONFIG_SET_GROUP,
	GATT_CONFIG_START_SYNC,
	GATT_CONFIG_HELLO,
	GATT_CONFIG_DONE,
};

struct peer_state {
	struct bt_conn *conn;
	struct bt_gatt_discover_params discover_params;
	struct bt_gatt_subscribe_params subscribe_params;
	uint16_t rx_handle;
	uint16_t tx_value_handle;
	atomic_t notify_subscribed;
	atomic_t configured;
	atomic_t write_inflight;
	enum gatt_config_step write_step;
	char write_text[96];
	bool in_use;
};

struct adv_match {
	char name[CONFIG_BT_DEVICE_NAME_MAX];
	bool service_uuid_matches;
};

static struct peer_state peers[CONFIG_BT_MAX_CONN];
static atomic_t connecting;
static atomic_t scan_running;
static atomic_t scan_seen;
static atomic_t scan_match;
static atomic_t scan_reject_type;
static atomic_t scan_reject_filter;
static atomic_t connect_attempts;
static atomic_t connect_failures;
static atomic_t gatt_tx_found;
static atomic_t gatt_tx_ccc_found;
static atomic_t gatt_subscribe_attempts;
static atomic_t gatt_subscribe_failures;
static atomic_t gatt_notify_subscribed;
static atomic_t gatt_rx_found;
static atomic_t gatt_write_attempts;
static atomic_t gatt_write_failures;
static atomic_t gatt_write_successes;
static atomic_t gatt_write_completions;
static atomic_t gatt_write_retries;
static atomic_t gatt_write_inflight;
static atomic_t gatt_write_step;
static atomic_t gatt_last_write_error;
static const uint8_t service_uuid_ad[] = { BT_UUID_TIME_SYNC_SERVICE_VAL };
static const struct bt_le_scan_param scan_param =
	BT_LE_SCAN_PARAM_INIT(BT_LE_SCAN_TYPE_ACTIVE, BT_LE_SCAN_OPT_NONE,
			      BT_GAP_SCAN_FAST_INTERVAL,
			      BT_GAP_SCAN_FAST_WINDOW);
static const struct bt_le_conn_param conn_param =
	BT_LE_CONN_PARAM_INIT(BT_GAP_MS_TO_CONN_INTERVAL(200),
			      BT_GAP_MS_TO_CONN_INTERVAL(250), 0,
			      BT_GAP_MS_TO_CONN_TIMEOUT(4000));

static void scan_start(void);
static void discover_tx(struct bt_conn *conn);
static void discover_rx_start(struct bt_conn *conn);
static void gatt_write_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(gatt_write_work, gatt_write_work_handler);

static struct peer_state *peer_for_conn(struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].in_use && peers[i].conn == conn) {
			return &peers[i];
		}
	}

	return NULL;
}

static struct peer_state *peer_alloc(struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (!peers[i].in_use) {
			memset(&peers[i], 0, sizeof(peers[i]));
			peers[i].conn = bt_conn_ref(conn);
			peers[i].write_step = GATT_CONFIG_DONE;
			peers[i].in_use = true;
			return &peers[i];
		}
	}

	return NULL;
}

static void clear_write_inflight(struct peer_state *peer)
{
	if (peer == NULL ||
	    atomic_cas(&peer->write_inflight, 1, 0) == false) {
		return;
	}

	if (atomic_get(&gatt_write_inflight) > 0) {
		atomic_dec(&gatt_write_inflight);
	}
}

static void peer_release(struct peer_state *peer)
{
	if (peer == NULL) {
		return;
	}

	clear_write_inflight(peer);

	if (peer->conn != NULL) {
		bt_conn_unref(peer->conn);
	}

	memset(peer, 0, sizeof(*peer));
}

static struct peer_state *peer_pending_write(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].in_use &&
		    peers[i].write_step != GATT_CONFIG_DONE) {
			return &peers[i];
		}
	}

	return NULL;
}

static bool write_error_is_retryable(int ret)
{
	return ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY;
}

static void schedule_gatt_write_retry(void)
{
	atomic_inc(&gatt_write_retries);
	(void)k_work_schedule(&gatt_write_work,
			      K_MSEC(GATT_WRITE_RETRY_DELAY_MS));
}

static void schedule_gatt_write_next(void)
{
	(void)k_work_schedule(&gatt_write_work,
			      K_MSEC(GATT_WRITE_NEXT_DELAY_MS));
}

static const char *write_step_text(struct peer_state *peer)
{
	if (peer == NULL) {
		return NULL;
	}

	switch (peer->write_step) {
	case GATT_CONFIG_SET_GROUP:
		snprintk(peer->write_text, sizeof(peer->write_text),
			 "SET_GROUP network=0x%08x channel=%u interval=%u",
			 TIME_SYNC_NETWORK_ID_VALUE,
			 TIME_SYNC_RF_CHANNEL_VALUE,
			 TIME_SYNC_INTERVAL_US_VALUE);
		return peer->write_text;
	case GATT_CONFIG_START_SYNC:
		return "START_SYNC";
	case GATT_CONFIG_HELLO:
		return "HELLO_FROM_MASTER hello world";
	case GATT_CONFIG_DONE:
	default:
		return NULL;
	}
}

static void gatt_write_complete(struct bt_conn *conn, void *user_data)
{
	struct peer_state *peer = user_data;

	if (peer == NULL || !peer->in_use || peer->conn != conn) {
		return;
	}

	clear_write_inflight(peer);
	atomic_inc(&gatt_write_completions);
	atomic_set(&gatt_last_write_error, 0);

	if (peer->write_step < GATT_CONFIG_DONE) {
		peer->write_step =
			(enum gatt_config_step)(peer->write_step + 1);
		atomic_set(&gatt_write_step, (atomic_val_t)peer->write_step);
	}

	if (peer->write_step == GATT_CONFIG_DONE) {
		LOG_INF("BLE group config and hello sent handle=%u",
			peer->rx_handle);
		return;
	}

	schedule_gatt_write_next();
}

static int write_rx_text(struct peer_state *peer, const char *text)
{
	int ret;

	if (peer == NULL || peer->conn == NULL || peer->rx_handle == 0u ||
	    text == NULL) {
		return -EINVAL;
	}

	atomic_inc(&gatt_write_attempts);
	atomic_set(&peer->write_inflight, 1);
	atomic_inc(&gatt_write_inflight);
	ret = bt_gatt_write_without_response_cb(peer->conn, peer->rx_handle,
						text, strlen(text), false,
						gatt_write_complete, peer);
	if (ret != 0) {
		clear_write_inflight(peer);
		atomic_inc(&gatt_write_failures);
		atomic_set(&gatt_last_write_error, ret);
		LOG_WRN("BLE write failed: %d step=%u text=%s", ret,
			(uint32_t)peer->write_step, text);
	} else {
		atomic_inc(&gatt_write_successes);
		LOG_INF("BLE write queued step=%u text=%s",
			(uint32_t)peer->write_step, text);
	}

	return ret;
}

static void gatt_write_work_handler(struct k_work *work)
{
	struct peer_state *peer;
	const char *text;
	int ret;

	ARG_UNUSED(work);

	peer = peer_pending_write();
	if (peer == NULL || peer->conn == NULL || peer->rx_handle == 0u ||
	    atomic_get(&peer->notify_subscribed) == 0 ||
	    peer->write_step == GATT_CONFIG_DONE ||
	    atomic_get(&peer->write_inflight) != 0) {
		return;
	}

	text = write_step_text(peer);
	if (text == NULL) {
		return;
	}

	atomic_set(&gatt_write_step, (atomic_val_t)peer->write_step);
	ret = write_rx_text(peer, text);
	if (ret != 0) {
		if (write_error_is_retryable(ret)) {
			schedule_gatt_write_retry();
		} else {
			atomic_clear_bit(&peer->configured, 0);
			LOG_WRN("BLE write stopped after non-retryable error: %d",
				ret);
		}
		return;
	}
}

static void write_group_config(struct peer_state *peer)
{
	if (peer == NULL || peer->conn == NULL || peer->rx_handle == 0u ||
	    atomic_get(&peer->notify_subscribed) == 0 ||
	    atomic_test_and_set_bit(&peer->configured, 0)) {
		return;
	}

	peer->write_step = GATT_CONFIG_SET_GROUP;
	atomic_set(&gatt_write_step, (atomic_val_t)peer->write_step);
	schedule_gatt_write_next();
}

static void subscribe_complete(struct bt_conn *conn, uint8_t err,
			       struct bt_gatt_subscribe_params *params)
{
	struct peer_state *peer = peer_for_conn(conn);

	ARG_UNUSED(params);

	if (peer == NULL) {
		return;
	}

	if (err != 0) {
		LOG_WRN("BLE notify subscribe failed att_err=%u", err);
		atomic_inc(&gatt_subscribe_failures);
		return;
	}

	atomic_set(&peer->notify_subscribed, 1);
	atomic_inc(&gatt_notify_subscribed);
	LOG_INF("BLE notify subscribe complete");
	discover_rx_start(conn);
}

static uint8_t notify_rx(struct bt_conn *conn,
			 struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	char msg[128];
	size_t copy_len;

	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (data == NULL) {
		return BT_GATT_ITER_STOP;
	}

	copy_len = MIN((size_t)length, sizeof(msg) - 1u);
	memcpy(msg, data, copy_len);
	msg[copy_len] = '\0';
	LOG_INF("BLE notify: %s", msg);

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_tx_ccc(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       struct bt_gatt_discover_params *params)
{
	struct peer_state *peer = peer_for_conn(conn);
	int ret;

	if (peer == NULL) {
		return BT_GATT_ITER_STOP;
	}

	if (attr == NULL) {
		memset(params, 0, sizeof(*params));
		LOG_WRN("BLE TX CCC not found");
		return BT_GATT_ITER_STOP;
	}

	atomic_inc(&gatt_tx_ccc_found);
	memset(params, 0, sizeof(*params));
	peer->subscribe_params.notify = notify_rx;
	peer->subscribe_params.subscribe = subscribe_complete;
	peer->subscribe_params.value = BT_GATT_CCC_NOTIFY;
	peer->subscribe_params.value_handle = peer->tx_value_handle;
	peer->subscribe_params.ccc_handle = attr->handle;

	atomic_inc(&gatt_subscribe_attempts);
	ret = bt_gatt_subscribe(conn, &peer->subscribe_params);
	if (ret != 0 && ret != -EALREADY) {
		atomic_inc(&gatt_subscribe_failures);
		LOG_WRN("BLE notify subscribe failed: %d", ret);
	} else {
		LOG_INF("BLE TX notify subscribe queued");
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t discover_tx_char(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	struct peer_state *peer = peer_for_conn(conn);
	int ret;

	if (peer == NULL) {
		return BT_GATT_ITER_STOP;
	}

	if (attr == NULL) {
		memset(params, 0, sizeof(*params));
		LOG_WRN("BLE TX characteristic not found");
		return BT_GATT_ITER_STOP;
	}

	atomic_inc(&gatt_tx_found);
	peer->tx_value_handle = bt_gatt_attr_value_handle(attr);
	memset(params, 0, sizeof(*params));

	peer->discover_params.uuid = BT_UUID_GATT_CCC;
	peer->discover_params.func = discover_tx_ccc;
	peer->discover_params.start_handle = peer->tx_value_handle + 1u;
	peer->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	peer->discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

	ret = bt_gatt_discover(conn, &peer->discover_params);
	if (ret != 0) {
		LOG_WRN("BLE TX CCC discover failed: %d", ret);
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t discover_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	struct peer_state *peer = peer_for_conn(conn);

	if (peer == NULL) {
		return BT_GATT_ITER_STOP;
	}

	if (attr == NULL) {
		memset(params, 0, sizeof(*params));
		LOG_WRN("BLE RX characteristic not found");
		return BT_GATT_ITER_STOP;
	}

	atomic_inc(&gatt_rx_found);
	peer->rx_handle = bt_gatt_attr_value_handle(attr);
	memset(params, 0, sizeof(*params));
	LOG_INF("BLE RX characteristic handle=%u", peer->rx_handle);
	write_group_config(peer);

	return BT_GATT_ITER_STOP;
}

static void discover_rx_start(struct bt_conn *conn)
{
	struct peer_state *peer = peer_for_conn(conn);
	int ret;

	if (peer == NULL) {
		return;
	}

	peer->discover_params.uuid = &bt_uuid_time_sync_rx.uuid;
	peer->discover_params.func = discover_rx;
	peer->discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	peer->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	peer->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	ret = bt_gatt_discover(conn, &peer->discover_params);
	if (ret != 0) {
		LOG_WRN("BLE RX discover failed: %d", ret);
	}
}

static void discover_tx(struct bt_conn *conn)
{
	struct peer_state *peer = peer_for_conn(conn);
	int ret;

	if (peer == NULL) {
		return;
	}

	peer->discover_params.uuid = &bt_uuid_time_sync_tx.uuid;
	peer->discover_params.func = discover_tx_char;
	peer->discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	peer->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	peer->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	ret = bt_gatt_discover(conn, &peer->discover_params);
	if (ret != 0) {
		LOG_WRN("BLE TX discover failed: %d", ret);
	}
}

static bool parse_ad_cb(struct bt_data *data, void *user_data)
{
	struct adv_match *match = user_data;
	size_t len;

	if (data->type == BT_DATA_NAME_SHORTENED ||
	    data->type == BT_DATA_NAME_COMPLETE) {
		len = MIN((size_t)data->data_len,
			  (size_t)sizeof(match->name) - 1u);
		memcpy(match->name, data->data, len);
		match->name[len] = '\0';
		return true;
	}

	if (data->type == BT_DATA_UUID128_SOME ||
	    data->type == BT_DATA_UUID128_ALL) {
		for (uint8_t pos = 0;
		     pos + sizeof(service_uuid_ad) <= data->data_len;
		     pos += sizeof(service_uuid_ad)) {
			if (memcmp(&data->data[pos], service_uuid_ad,
				   sizeof(service_uuid_ad)) == 0) {
				match->service_uuid_matches = true;
				return true;
			}
		}
	}

	return true;
}

static bool name_matches_slave(const char *name)
{
	char expected[CONFIG_BT_DEVICE_NAME_MAX];

	snprintk(expected, sizeof(expected), "%s%s",
		 TIME_SYNC_BLE_DEVICE_NAME_PREFIX_VALUE, SLAVE_NAME_SUFFIX);

	return strcmp(name, expected) == 0;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct adv_match match = {0};
	char addr_str[BT_ADDR_LE_STR_LEN];
	struct bt_conn *conn = NULL;
	int ret;

	if (atomic_get(&connecting) != 0) {
		return;
	}

	atomic_inc(&scan_seen);

	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		atomic_inc(&scan_reject_type);
		return;
	}

	bt_data_parse(ad, parse_ad_cb, &match);
	if (!match.service_uuid_matches && !name_matches_slave(match.name)) {
		atomic_inc(&scan_reject_filter);
		return;
	}

	atomic_inc(&scan_match);

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	LOG_INF("BLE slave found %s rssi=%d name=%s", addr_str, rssi,
		match.name);

	ret = bt_le_scan_stop();
	if (ret != 0) {
		return;
	}

	atomic_set(&scan_running, 0);
	atomic_set(&connecting, 1);
	atomic_inc(&connect_attempts);

	ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				&conn_param, &conn);
	if (ret != 0) {
		LOG_WRN("BLE create connection failed: %d", ret);
		atomic_inc(&connect_failures);
		atomic_set(&connecting, 0);
		scan_start();
		return;
	}

	if (conn != NULL) {
		bt_conn_unref(conn);
	}
}

static void scan_start(void)
{
	int ret;

	if (atomic_get(&scan_running) != 0) {
		return;
	}

	ret = bt_le_scan_start(&scan_param, device_found);
	if (ret != 0) {
		LOG_WRN("BLE scan start failed: %d", ret);
		return;
	}

	atomic_set(&scan_running, 1);
	LOG_INF("BLE scan started");
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	struct peer_state *peer;

	atomic_set(&connecting, 0);

	if (err != 0) {
		LOG_WRN("BLE central connect failed: %u", err);
		atomic_inc(&connect_failures);
		scan_start();
		return;
	}

	peer = peer_alloc(conn);
	if (peer == NULL) {
		LOG_WRN("BLE peer table full");
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		scan_start();
		return;
	}

	LOG_INF("BLE central connected");
	discover_tx(conn);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	struct peer_state *peer = peer_for_conn(conn);

	ARG_UNUSED(reason);

	peer_release(peer);
	scan_start();
}

BT_CONN_CB_DEFINE(client_conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};
#endif

int ble_time_sync_client_start(void)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	scan_start();
	return 0;
#else
	return -ENOTSUP;
#endif
}

bool ble_time_sync_client_has_peer(void)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	return ble_time_sync_client_peer_count() > 0u;
#endif

	return false;
}

uint8_t ble_time_sync_client_peer_count(void)
{
	uint8_t count = 0u;

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].in_use) {
			count++;
		}
	}
#endif

	return count;
}

bool ble_time_sync_client_notify_subscribed(void)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].in_use &&
		    atomic_get(&peers[i].notify_subscribed) != 0) {
			return true;
		}
	}
#endif

	return false;
}

bool ble_time_sync_client_scanning(void)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	return atomic_get(&scan_running) != 0;
#else
	return false;
#endif
}

void ble_time_sync_client_get_scan_stats(
	struct ble_time_sync_client_scan_stats *stats)
{
	if (stats == NULL) {
		return;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	stats->scan_seen = (uint32_t)atomic_get(&scan_seen);
	stats->scan_match = (uint32_t)atomic_get(&scan_match);
	stats->scan_reject_type = (uint32_t)atomic_get(&scan_reject_type);
	stats->scan_reject_filter =
		(uint32_t)atomic_get(&scan_reject_filter);
	stats->connect_attempts = (uint32_t)atomic_get(&connect_attempts);
	stats->connect_failures = (uint32_t)atomic_get(&connect_failures);
	stats->gatt_tx_found = (uint32_t)atomic_get(&gatt_tx_found);
	stats->gatt_tx_ccc_found = (uint32_t)atomic_get(&gatt_tx_ccc_found);
	stats->gatt_subscribe_attempts =
		(uint32_t)atomic_get(&gatt_subscribe_attempts);
	stats->gatt_subscribe_failures =
		(uint32_t)atomic_get(&gatt_subscribe_failures);
	stats->gatt_notify_subscribed =
		(uint32_t)atomic_get(&gatt_notify_subscribed);
	stats->gatt_rx_found = (uint32_t)atomic_get(&gatt_rx_found);
	stats->gatt_write_attempts =
		(uint32_t)atomic_get(&gatt_write_attempts);
	stats->gatt_write_failures =
		(uint32_t)atomic_get(&gatt_write_failures);
	stats->gatt_write_successes =
		(uint32_t)atomic_get(&gatt_write_successes);
	stats->gatt_write_completions =
		(uint32_t)atomic_get(&gatt_write_completions);
	stats->gatt_write_retries =
		(uint32_t)atomic_get(&gatt_write_retries);
	stats->gatt_write_inflight =
		(uint32_t)atomic_get(&gatt_write_inflight);
	stats->gatt_write_step =
		(uint32_t)atomic_get(&gatt_write_step);
	stats->gatt_last_write_error =
		(int)atomic_get(&gatt_last_write_error);
#else
	memset(stats, 0, sizeof(*stats));
#endif
}
