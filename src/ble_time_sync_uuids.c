#include "ble_time_sync_uuids.h"

const struct bt_uuid_128 bt_uuid_time_sync_service =
	BT_UUID_INIT_128(BT_UUID_TIME_SYNC_SERVICE_VAL);
const struct bt_uuid_128 bt_uuid_time_sync_rx =
	BT_UUID_INIT_128(BT_UUID_TIME_SYNC_RX_VAL);
const struct bt_uuid_128 bt_uuid_time_sync_tx =
	BT_UUID_INIT_128(BT_UUID_TIME_SYNC_TX_VAL);
const struct bt_uuid_128 bt_uuid_time_sync_status =
	BT_UUID_INIT_128(BT_UUID_TIME_SYNC_STATUS_VAL);
