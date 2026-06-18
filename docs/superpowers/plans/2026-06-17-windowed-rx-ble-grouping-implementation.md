# Windowed RX and BLE Grouping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add MPSL windowed slave RX and BLE scheme B grouping where master acts as BLE Central and slave acts as BLE Peripheral.

**Architecture:** Keep PPS and proprietary RADIO timestamping unchanged. Add a runtime sync configuration object shared by main/radio/BLE. Replace slave continuous RX with acquire and locked prediction windows. Add a small custom UART-like GATT service on slave and a master GATT client that writes group commands.

**Tech Stack:** nRF Connect SDK v3.3.1, Zephyr Bluetooth Host, Nordic SoftDevice Controller/MPSL, nRF52840 RADIO/TIMER/PPI/GPIOTE.

---

## File Structure

- Modify `Kconfig`: add BLE enable, BLE name, RX window parameters, runtime config limits.
- Modify `prj.conf`: enable Bluetooth Host, Central/Peripheral role symbols, GATT client, scan, MPSL coexistence settings.
- Modify `master.conf`: enable master BLE Central behavior.
- Modify `slave.conf`: enable slave BLE Peripheral behavior and keep radio delay calibration.
- Modify `CMakeLists.txt`: add new source files.
- Create `src/runtime_config.h` and `src/runtime_config.c`: owns current network/channel/interval and applies BLE SET_GROUP updates.
- Modify `src/app_config.h`: expose new Kconfig defaults.
- Modify `src/radio_sync.h` and `src/radio_sync.c`: support runtime reconfigure, start/stop RX, acquire/locked window scheduling and stats.
- Create `src/ble_time_sync.h` and `src/ble_time_sync.c`: common BLE init wrapper and status access.
- Create `src/ble_time_sync_service.h` and `src/ble_time_sync_service.c`: slave custom GATT service and command parser.
- Create `src/ble_time_sync_client.h` and `src/ble_time_sync_client.c`: master scanner/central/GATT client for scheme B.
- Modify `src/main.c`: use runtime config, drive window updates from sync filter, integrate BLE start and SET_GROUP handling.
- Modify `src/status.h` and `src/status.c`: log BLE and RX window diagnostics.
- Modify `README.md`: document BLE pairing/grouping and window RX parameters.

## Task 1: Runtime Configuration and Kconfig

**Files:**
- Modify: `Kconfig`
- Modify: `prj.conf`
- Modify: `master.conf`
- Modify: `slave.conf`
- Modify: `CMakeLists.txt`
- Modify: `src/app_config.h`
- Create: `src/runtime_config.h`
- Create: `src/runtime_config.c`

- [ ] **Step 1: Add Kconfig options**

Add these options under the existing time sync menu in `Kconfig`:

```kconfig
config TIME_SYNC_BLE
	bool "Enable Bluetooth control channel"
	default y

config TIME_SYNC_BLE_DEVICE_NAME_PREFIX
	string "Bluetooth device name prefix"
	default "TS"
	depends on TIME_SYNC_BLE

config TIME_SYNC_RX_ACQUIRE_WINDOW_US
	int "Slave acquire RX window length in microseconds"
	default 12000
	range 1000 100000

config TIME_SYNC_RX_ACQUIRE_PERIOD_US
	int "Slave acquire RX window period in microseconds"
	default 100000
	range 10000 1000000

config TIME_SYNC_RX_LOCKED_PRE_MARGIN_US
	int "Slave locked RX pre-margin before predicted beacon"
	default 1500
	range 100 20000

config TIME_SYNC_RX_LOCKED_POST_MARGIN_US
	int "Slave locked RX post-margin after predicted beacon"
	default 2500
	range 100 20000

config TIME_SYNC_RX_WINDOW_GUARD_US
	int "Slave RX window guard time in microseconds"
	default 300
	range 0 5000

config TIME_SYNC_RX_MISSED_TO_ACQUIRE
	int "Missed locked windows before returning to acquire mode"
	default 3
	range 1 20
```

- [ ] **Step 2: Enable Bluetooth in project configuration**

Update `prj.conf` with common Bluetooth settings:

```conf
CONFIG_BT=y
CONFIG_BT_DEVICE_NAME_DYNAMIC=y
CONFIG_BT_MAX_CONN=4
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_ATT_PREPARE_COUNT=2
CONFIG_BT_GATT_CLIENT=y
CONFIG_BT_SCAN=y
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1
```

Keep existing `CONFIG_MPSL=y` and logging settings.

- [ ] **Step 3: Set role-specific Bluetooth options**

Update `master.conf`:

```conf
CONFIG_TIME_SYNC_ROLE_MASTER=y
CONFIG_BT_CENTRAL=y
CONFIG_BT_OBSERVER=y
CONFIG_BT_PERIPHERAL=n
```

Update `slave.conf` while preserving `CONFIG_TIME_SYNC_RADIO_DELAY_US=17`:

```conf
CONFIG_TIME_SYNC_ROLE_SLAVE=y
CONFIG_TIME_SYNC_RADIO_DELAY_US=17
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_BROADCASTER=y
CONFIG_BT_CENTRAL=n
CONFIG_BT_OBSERVER=n
```

- [ ] **Step 4: Add app config macros**

Extend `src/app_config.h`:

```c
#define TIME_SYNC_RX_ACQUIRE_WINDOW_US_VALUE CONFIG_TIME_SYNC_RX_ACQUIRE_WINDOW_US
#define TIME_SYNC_RX_ACQUIRE_PERIOD_US_VALUE CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US
#define TIME_SYNC_RX_LOCKED_PRE_MARGIN_US_VALUE CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US
#define TIME_SYNC_RX_LOCKED_POST_MARGIN_US_VALUE CONFIG_TIME_SYNC_RX_LOCKED_POST_MARGIN_US
#define TIME_SYNC_RX_WINDOW_GUARD_US_VALUE CONFIG_TIME_SYNC_RX_WINDOW_GUARD_US
#define TIME_SYNC_RX_MISSED_TO_ACQUIRE_VALUE CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE
#define TIME_SYNC_BLE_DEVICE_NAME_PREFIX_VALUE CONFIG_TIME_SYNC_BLE_DEVICE_NAME_PREFIX
```

- [ ] **Step 5: Create runtime_config API**

Create `src/runtime_config.h`:

```c
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
```

Create `src/runtime_config.c`:

```c
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
```

- [ ] **Step 6: Add runtime_config to build**

Add `src/runtime_config.c` to `target_sources(app PRIVATE ...)` in `CMakeLists.txt`.

- [ ] **Step 7: Verify configuration builds far enough**

Run:

```bash
./build.sh master
```

Expected: build may fail later because BLE source files are not added yet only if references were added prematurely. If only Task 1 is applied, master build should pass.

Run:

```bash
./build.sh slave
```

Expected: same as master.

## Task 2: Windowed RX in radio_sync

**Files:**
- Modify: `src/radio_sync.h`
- Modify: `src/radio_sync.c`
- Modify: `src/main.c`
- Modify: `src/status.c`

- [ ] **Step 1: Extend radio_sync API**

In `src/radio_sync.h`, add:

```c
enum radio_sync_rx_mode {
	RADIO_SYNC_RX_STOPPED,
	RADIO_SYNC_RX_ACQUIRE,
	RADIO_SYNC_RX_LOCKED_WINDOW,
};

struct radio_sync_window {
	uint64_t start_tick;
	uint32_t length_us;
	enum radio_sync_rx_mode mode;
};
```

Extend `struct radio_sync_stats`:

```c
	uint32_t rx_windows;
	uint32_t rx_window_skips;
	uint32_t rx_window_late;
	uint32_t rx_window_length_us;
	enum radio_sync_rx_mode rx_mode;
```

Add functions:

```c
int radio_sync_reconfigure(uint8_t channel, uint32_t network_id);
int radio_sync_stop_rx(void);
int radio_sync_schedule_rx_window(const struct radio_sync_window *window);
```

- [ ] **Step 2: Implement stop and reconfigure**

In `src/radio_sync.c`, add state for requested RX window:

```c
static uint64_t requested_rx_start_tick;
static uint32_t requested_rx_length_us;
static enum radio_sync_rx_mode requested_rx_mode = RADIO_SYNC_RX_STOPPED;
```

Implement:

```c
int radio_sync_reconfigure(uint8_t channel, uint32_t network_id)
{
	configured_channel = channel;
	configured_network_id = network_id;
	return 0;
}

int radio_sync_stop_rx(void)
{
	rx_enabled = false;
	if (requested_mode == TIMESLOT_MODE_RX) {
		requested_mode = TIMESLOT_MODE_IDLE;
	}
	stats.rx_mode = RADIO_SYNC_RX_STOPPED;
	return 0;
}
```

- [ ] **Step 3: Replace continuous RX start with acquire wrapper**

Change `radio_sync_start_rx()` so it schedules an acquire window instead of continuous 100 ms RX:

```c
int radio_sync_start_rx(void)
{
	struct radio_sync_window window = {
		.start_tick = timebase_now_us() + 2000u,
		.length_us = TIME_SYNC_RX_ACQUIRE_WINDOW_US_VALUE,
		.mode = RADIO_SYNC_RX_ACQUIRE,
	};

	return radio_sync_schedule_rx_window(&window);
}
```

- [ ] **Step 4: Implement radio_sync_schedule_rx_window**

Add implementation:

```c
int radio_sync_schedule_rx_window(const struct radio_sync_window *window)
{
	int ret;
	uint64_t now = timebase_now_us();

	if (window == NULL || window->length_us < RADIO_TIMESLOT_MIN_LENGTH_US ||
	    window->length_us > MPSL_TIMESLOT_LENGTH_MAX_US) {
		return -EINVAL;
	}

	if (!timeslot_session_opened) {
		return -EACCES;
	}

	if (window->start_tick <= now + RADIO_TX_ARM_AHEAD_US) {
		stats.rx_window_late++;
		return -ETIME;
	}

	if (requested_mode != TIMESLOT_MODE_IDLE &&
	    requested_mode != TIMESLOT_MODE_RX) {
		stats.busy_errors++;
		return -EBUSY;
	}

	requested_rx_start_tick = window->start_tick;
	requested_rx_length_us = window->length_us;
	requested_rx_mode = window->mode;
	stats.rx_window_length_us = window->length_us;
	stats.rx_mode = window->mode;
	rx_enabled = true;
	requested_mode = TIMESLOT_MODE_RX;

	timeslot_request_earliest.params.earliest.length_us = window->length_us;
	timeslot_request_normal.params.normal.length_us = window->length_us;

	ret = mpsl_timeslot_request(timeslot_session_id,
				    &timeslot_request_earliest);
	if (ret != 0 && ret != -NRF_EAGAIN) {
		stats.busy_errors++;
		return ret;
	}

	return 0;
}
```

If this first implementation cannot request an absolute start with MPSL normal distance yet, keep earliest scheduling for acquire and locked windows, then tighten in the follow-up step by computing normal distance from prior slot start.

- [ ] **Step 5: Stop auto-requesting continuous normal RX**

In `timeslot_timer_action()`, replace the current RX branch:

```c
if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
	return request_next_from_callback(&timeslot_request_normal);
}
```

with:

```c
if (requested_mode == TIMESLOT_MODE_RX) {
	requested_mode = TIMESLOT_MODE_IDLE;
	stats.rx_windows++;
	return end_from_callback();
}
```

This ensures one request opens one RX window.

- [ ] **Step 6: Schedule acquire/locked windows from slave_loop**

In `src/main.c`, add slave-side state:

```c
uint64_t next_rx_window = now + 2000u;
uint32_t consecutive_missed_windows = 0;
enum radio_sync_rx_mode rx_mode = RADIO_SYNC_RX_ACQUIRE;
```

On startup schedule acquire window. After every valid RX, compute next locked window:

```c
uint64_t next_master_tx = rx.beacon.master_tx_tick + cfg.sync_interval_us;
uint64_t predicted_local;
if (sync_filter_master_to_local(&filter, next_master_tx, &predicted_local) == 0) {
	struct radio_sync_window win = {
		.start_tick = predicted_local - TIME_SYNC_RX_LOCKED_PRE_MARGIN_US_VALUE,
		.length_us = TIME_SYNC_RX_LOCKED_PRE_MARGIN_US_VALUE +
			     TIME_SYNC_RX_LOCKED_POST_MARGIN_US_VALUE +
			     TIME_SYNC_RX_WINDOW_GUARD_US_VALUE,
		.mode = RADIO_SYNC_RX_LOCKED_WINDOW,
	};
	(void)radio_sync_schedule_rx_window(&win);
	rx_mode = RADIO_SYNC_RX_LOCKED_WINDOW;
	consecutive_missed_windows = 0;
}
```

If no packet arrives by expected window end, increment `consecutive_missed_windows`. If it reaches `TIME_SYNC_RX_MISSED_TO_ACQUIRE_VALUE`, schedule acquire windows again.

- [ ] **Step 7: Add status string for rx_mode**

In `src/status.c`, map `RADIO_SYNC_RX_STOPPED`, `RADIO_SYNC_RX_ACQUIRE`, `RADIO_SYNC_RX_LOCKED_WINDOW` to strings and append:

```c
" rx_mode=%s rx_win=%u rx_open=%u rx_skip=%u rx_late=%u"
```

- [ ] **Step 8: Build both roles**

Run:

```bash
./build.sh master
./build.sh slave
```

Expected: both builds pass. Hardware behavior expected before BLE: slave still locks; logs show `rx_mode=locked_window` after lock.

## Task 3: Slave BLE Peripheral GATT Service

**Files:**
- Create: `src/ble_time_sync.h`
- Create: `src/ble_time_sync.c`
- Create: `src/ble_time_sync_service.h`
- Create: `src/ble_time_sync_service.c`
- Modify: `CMakeLists.txt`
- Modify: `src/main.c`

- [ ] **Step 1: Create common BLE header**

Create `src/ble_time_sync.h`:

```c
#ifndef BLE_TIME_SYNC_H
#define BLE_TIME_SYNC_H

#include <stdbool.h>

int ble_time_sync_init(void);
bool ble_time_sync_connected(void);

#endif
```

- [ ] **Step 2: Create slave service header**

Create `src/ble_time_sync_service.h`:

```c
#ifndef BLE_TIME_SYNC_SERVICE_H
#define BLE_TIME_SYNC_SERVICE_H

#include <stddef.h>

typedef void (*ble_time_sync_command_handler_t)(const char *cmd, size_t len);

int ble_time_sync_service_init(ble_time_sync_command_handler_t handler);
int ble_time_sync_service_notify(const char *msg);
const char *ble_time_sync_service_status(void);

#endif
```

- [ ] **Step 3: Implement BLE init for slave Peripheral**

Create `src/ble_time_sync.c` with role-gated initialization:

```c
#include "ble_time_sync.h"

#include "app_config.h"
#include "ble_time_sync_service.h"

#include <stdio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_time_sync, LOG_LEVEL_INF);

static bool connected;
static struct bt_conn *default_conn;

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		LOG_WRN("BLE connect failed: %u", err);
		return;
	}
	default_conn = bt_conn_ref(conn);
	connected = true;
	LOG_INF("BLE connected");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	connected = false;
	if (default_conn != NULL) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}
	LOG_INF("BLE disconnected reason=%u", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

bool ble_time_sync_connected(void)
{
	return connected;
}

int ble_time_sync_init(void)
{
	int ret = bt_enable(NULL);

	if (ret != 0) {
		return ret;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	char name[32];
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	};

	snprintf(name, sizeof(name), "%s-SLAVE", TIME_SYNC_BLE_DEVICE_NAME_PREFIX_VALUE);
	(void)bt_set_name(name);

	ret = ble_time_sync_service_init(NULL);
	if (ret != 0) {
		return ret;
	}

	ret = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("BLE advertising as %s", name);
#endif

	return 0;
}
```

- [ ] **Step 4: Implement GATT service skeleton**

Create `src/ble_time_sync_service.c` with custom UUIDs, RX write callback, TX CCC and STATUS read. Use `BT_GATT_SERVICE_DEFINE`.

The RX write callback must copy at most 127 bytes into a null-terminated buffer and call the registered handler if non-NULL.

The STATUS read callback returns:

```text
role=slave conn=1
```

The notify function calls `bt_gatt_notify(NULL, &time_sync_svc.attrs[3], msg, strlen(msg))` using the TX value attribute index from the service definition.

- [ ] **Step 5: Wire BLE init into main**

In `src/main.c`, include `ble_time_sync.h` and call:

```c
ret = ble_time_sync_init();
if (ret != 0) {
	LOG_WRN("BLE init failed: %d", ret);
}
```

after timebase/PPS/radio initialization and before role loop.

- [ ] **Step 6: Add BLE sources**

Add `src/ble_time_sync.c` and `src/ble_time_sync_service.c` to `CMakeLists.txt`.

- [ ] **Step 7: Build slave**

Run:

```bash
./build.sh slave
```

Expected: build passes. Hardware check: slave advertises as `TS-SLAVE`.

## Task 4: BLE Command Handling on Slave

**Files:**
- Modify: `src/ble_time_sync.c`
- Modify: `src/ble_time_sync_service.c`
- Modify: `src/main.c`
- Modify: `src/runtime_config.c`
- Modify: `src/runtime_config.h`

- [ ] **Step 1: Add command event queue**

Add to `src/ble_time_sync.h`:

```c
struct ble_time_sync_command {
	char text[128];
};

int ble_time_sync_get_command(struct ble_time_sync_command *cmd);
```

In `src/ble_time_sync.c`, add `K_MSGQ_DEFINE(ble_cmd_msgq, sizeof(struct ble_time_sync_command), 4, 4);` and a command handler that pushes writes into the queue.

- [ ] **Step 2: Register service command handler**

Change slave init:

```c
ret = ble_time_sync_service_init(command_received);
```

where `command_received()` copies the command text into the msgq.

- [ ] **Step 3: Parse SET_GROUP in main loop**

In slave loop, poll `ble_time_sync_get_command()` each iteration. Implement handlers:

```c
GET_STATUS -> ble_time_sync_service_notify("OK STATUS ...")
SET_GROUP network=0x... channel=... interval=... -> runtime_config_set()
START_SYNC -> radio_sync_reconfigure(); reset sync_filter; schedule acquire
STOP_SYNC -> radio_sync_stop_rx()
```

Parsing can use `sscanf()` with exact format:

```c
sscanf(cmd.text, "SET_GROUP network=%x channel=%hhu interval=%u",
       &network, &channel, &interval)
```

- [ ] **Step 4: Build slave**

Run:

```bash
./build.sh slave
```

Expected: build passes. Hardware check with nRF Connect: write `GET_STATUS`, `SET_GROUP ...`, `START_SYNC` and receive `OK` notifications.

## Task 5: Master BLE Central Client

**Files:**
- Create: `src/ble_time_sync_client.h`
- Create: `src/ble_time_sync_client.c`
- Modify: `src/ble_time_sync.c`
- Modify: `CMakeLists.txt`
- Modify: `src/main.c`

- [ ] **Step 1: Create client header**

Create `src/ble_time_sync_client.h`:

```c
#ifndef BLE_TIME_SYNC_CLIENT_H
#define BLE_TIME_SYNC_CLIENT_H

int ble_time_sync_client_start(void);
bool ble_time_sync_client_has_peer(void);

#endif
```

- [ ] **Step 2: Implement scanner and connector**

In `src/ble_time_sync_client.c`, implement:

- `bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found)`
- filter by connectable advertisement and name prefix `TS-SLAVE`
- call `bt_conn_le_create()` once when no active connection exists

- [ ] **Step 3: Implement minimal GATT discovery and writes**

After connected:

- discover the custom service UUID
- discover RX characteristic value handle
- write:

```text
SET_GROUP network=0x%08x channel=%u interval=%u
START_SYNC
```

Use `bt_gatt_write_without_response()` for the first version after handle discovery.

- [ ] **Step 4: Start client on master**

In `ble_time_sync_init()`, for master role call `ble_time_sync_client_start()` after `bt_enable()`.

- [ ] **Step 5: Add source and build**

Add `src/ble_time_sync_client.c` to `CMakeLists.txt`.

Run:

```bash
./build.sh master
./build.sh slave
```

Expected: both builds pass. Hardware check: master logs show scan/connect/write; slave logs show SET_GROUP/START_SYNC.

## Task 6: Documentation, Diagnostics, and Final Verification

**Files:**
- Modify: `README.md`
- Modify: `src/status.h`
- Modify: `src/status.c`
- Modify: `src/ble_time_sync.c`

- [ ] **Step 1: Add BLE status to status snapshot**

Extend `struct status_snapshot`:

```c
	bool ble_connected;
```

Set from `ble_time_sync_connected()` in master and slave snapshots.

- [ ] **Step 2: Log BLE status**

Append to status log:

```c
" ble_conn=%u"
```

- [ ] **Step 3: Update README**

Document:

- master is BLE Central
- slave is BLE Peripheral
- pairing flow
- commands
- window RX defaults
- expected logs

- [ ] **Step 4: Run verification**

Run:

```bash
git diff --check
./build.sh master
./build.sh slave
```

Expected: all commands exit 0.

- [ ] **Step 5: Hardware verification**

Program one master and one slave:

- slave advertises `TS-SLAVE`
- master connects and writes group config
- slave locks and enters `rx_mode=locked_window`
- BLE remains connected for at least 2 minutes
- PPS remains close to prior 1-2 us measurement

- [ ] **Step 6: Commit implementation**

Do not add `tests/` unless explicitly requested.

```bash
git add Kconfig prj.conf master.conf slave.conf CMakeLists.txt README.md src
git commit -m "Add BLE grouping and windowed slave RX"
```
