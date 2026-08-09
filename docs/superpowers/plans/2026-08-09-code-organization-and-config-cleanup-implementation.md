# Code Organization and Configuration Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除退役实现和过程型测试，按功能重组源码/测试，收敛 NVS 与低价值 Kconfig，并将 README 精简为快速开始和工程介绍两部分。

**Architecture:** 保持现行 ESB 星型协议、PPS、UART bridge 和恢复行为不变；用 `app/config/bridge/radio/time/validation` 目录表达模块边界。运行时只保留八个部署参数，协议时序和资源容量保留为有价值的编译期配置或固定设计常量。

**Tech Stack:** Zephyr/NCS 3.3.1、C、CMake/Kconfig、Ztest native_sim、Python unittest、双 XIAO nRF52840 Plus、UF2/CDC。

---

### Task 1: Remove retired implementations and process tests

**Files:**
- Delete: `src/ble_time_sync*.c/.h`
- Delete: `src/radio_sync.c/.h`
- Delete: `src/runtime_config.c/.h`
- Delete: `src/sync_packet.c/.h`
- Delete: `tests/*_static_check.sh`
- Modify: `tests/sync_logic/CMakeLists.txt`
- Modify: `tests/sync_logic/src/main.c`

- [ ] **Step 1: Record pre-delete reachability evidence**

Run:

```bash
rg -n 'ble_time_sync|radio_sync|runtime_config|sync_packet' \
  CMakeLists.txt src tests -g '!src/ble_time_sync*'
```

Expected: production `CMakeLists.txt` has no deleted `.c` source; remaining references are retired/static checks or the legacy `sync_packet` native suite.

- [ ] **Step 2: Remove legacy packet tests**

In `tests/sync_logic/src/main.c`, delete `#include "sync_packet.h"`, the three `ZTEST(sync_packet, ...)` functions and `ZTEST_SUITE(sync_packet, ...)`.

Replace the source list in `tests/sync_logic/CMakeLists.txt` with:

```cmake
target_sources(app PRIVATE
	src/main.c
	../../src/sync_filter.c
	../../src/time_sync_math.c
)
```

- [ ] **Step 3: Delete retired code and all source-text static checks**

Run:

```bash
git rm \
  src/ble_time_sync.c src/ble_time_sync.h \
  src/ble_time_sync_client.c src/ble_time_sync_client.h \
  src/ble_time_sync_service.c src/ble_time_sync_service.h \
  src/ble_time_sync_uuids.c src/ble_time_sync_uuids.h \
  src/radio_sync.c src/radio_sync.h \
  src/runtime_config.c src/runtime_config.h \
  src/sync_packet.c src/sync_packet.h
git rm tests/*_static_check.sh
```

- [ ] **Step 4: Run native/host tests and both firmware builds**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/bridge_logic/zephyr/zephyr.exe
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native tests/sync_logic -d build/tests/sync_logic
./build/tests/sync_logic/sync_logic/zephyr/zephyr.exe
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native tests/time_uart_logic -d build/tests/time_uart_logic
./build/tests/time_uart_logic/time_uart_logic/zephyr/zephyr.exe
/home/hv/ncs/.venv/bin/python -m unittest -v tests/test_board_e2e.py tests/test_flash_uf2.py
./build.sh master -d build/cleanup-stage1-production
./build.sh master -d build/cleanup-stage1-validation -- -DOVERLAY_CONFIG=validation.conf
```

Expected: all commands exit 0; sync suite has exactly three fewer tests.

- [ ] **Step 5: Run stage-1 hardware gate**

```bash
/usr/bin/timeout 240s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage cleanup-stage1-dead-code \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/cleanup-stage1-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/cleanup-stage1-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --steady-state-seconds 5 --command-timeout 15
```

Expected: `BOARD_E2E PASS`, `active_count=1`, slave `LOCKED`, both 600-byte directions exact, zero retry/recovery/drop/error.

- [ ] **Step 6: Commit stage 1**

```bash
git diff --check
git add CMakeLists.txt src tests
git commit -m "refactor: remove retired radio and process tests"
```

### Task 2: Reorganize source and tests by function

**Files:**
- Modify: `CMakeLists.txt`
- Move: active `src/*` into six functional directories
- Move: native tests into `tests/unit/{bridge,config,radio,time,time_uart}`
- Move: Python tests into `tests/host/`

- [ ] **Step 1: Move active source files**

```bash
mkdir -p src/app src/config src/bridge src/radio src/time src/validation
git mv src/main.c src/status.c src/status.h src/heartbeat_led.c src/heartbeat_led.h src/app/
git mv src/app_config.h src/bridge_config.c src/bridge_config.h src/param_config.c src/param_config.h src/param_defs.h src/param_shell.c src/config/
git mv src/bridge_runtime.c src/bridge_runtime.h src/byte_ring.c src/byte_ring.h src/record_queue.c src/record_queue.h src/link_protocol.c src/link_protocol.h src/link_window.c src/link_window.h src/membership.c src/membership.h src/link_scheduler_core.c src/link_scheduler_core.h src/uart_bridge.c src/uart_bridge.h src/bridge/
git mv src/radio_address.c src/radio_address.h src/radio_address_crypto.c src/radio_address_crypto.h src/radio_transport.c src/radio_transport.h src/radio/
git mv src/nmea_parser.c src/nmea_parser.h src/pps_input.c src/pps_input.h src/pps_output.c src/pps_output.h src/sync_filter.c src/sync_filter.h src/sync_tracker.c src/sync_tracker.h src/time_sync_math.c src/time_sync_math.h src/time_sync_shell.c src/time_uart.c src/time_uart.h src/timebase.c src/timebase.h src/utc_clock.c src/utc_clock.h src/wireless_time_sync.c src/wireless_time_sync.h src/time/
git mv src/bridge_validation.c src/bridge_validation.h src/bridge_validation_shell.c src/validation/
```

- [ ] **Step 2: Replace root include/source maps**

Use these include directories in `CMakeLists.txt`:

```cmake
target_include_directories(app PRIVATE
	src/app src/config src/bridge src/radio src/time src/validation
)
```

Replace the application source blocks with:

```cmake
target_sources(app PRIVATE
	src/config/bridge_config.c
	src/config/param_config.c
	src/config/param_shell.c
	src/bridge/byte_ring.c
	src/bridge/record_queue.c
	src/bridge/link_protocol.c
	src/bridge/link_window.c
	src/bridge/membership.c
	src/bridge/link_scheduler_core.c
	src/bridge/uart_bridge.c
	src/bridge/bridge_runtime.c
	src/radio/radio_address.c
	src/radio/radio_address_crypto.c
	src/radio/radio_transport.c
	src/time/wireless_time_sync.c
	src/time/sync_tracker.c
	src/time/sync_filter.c
	src/time/time_sync_math.c
	src/time/timebase.c
	src/time/pps_output.c
	src/time/pps_input.c
	src/time/time_uart.c
	src/time/nmea_parser.c
	src/time/utc_clock.c
	src/time/time_sync_shell.c
	src/app/status.c
	src/app/heartbeat_led.c
	src/app/main.c
)

target_sources_ifdef(CONFIG_RADIO_BRIDGE_VALIDATION_CDC app PRIVATE
	src/validation/bridge_validation.c
	src/validation/bridge_validation_shell.c
)
```

- [ ] **Step 3: Move and split native tests**

```bash
mkdir -p tests/unit tests/host
git mv tests/bridge_logic tests/unit/bridge
git mv tests/sync_logic tests/unit/time
git mv tests/time_uart_logic tests/unit/time_uart
git mv tests/unit/time/src/main.c tests/unit/time/src/test_time_sync.c
mkdir -p tests/unit/config/src tests/unit/radio/src
git mv tests/unit/bridge/src/test_config.c tests/unit/config/src/
git mv tests/unit/bridge/src/test_param_config.c tests/unit/config/src/
git mv tests/unit/bridge/src/test_radio_address.c tests/unit/radio/src/
git mv tests/unit/bridge/src/test_nmea_parser.c tests/unit/time/src/
git mv tests/unit/bridge/src/test_utc_clock.c tests/unit/time/src/
git mv tests/unit/bridge/src/test_sync_tracker.c tests/unit/time/src/
git mv tests/unit/bridge/src/test_wireless_time_sync.c tests/unit/time/src/
```

Create `tests/unit/config/CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(config_tests)

target_include_directories(app PRIVATE
	../../../src/config
	../../../src/bridge
)
target_compile_definitions(app PRIVATE
	CONFIG_RADIO_BRIDGE_GROUP_ID=1
	CONFIG_RADIO_BRIDGE_GROUP_KEY="00112233445566778899aabbccddeeff"
	CONFIG_RADIO_BRIDGE_UART_BAUDRATE=115200
	CONFIG_RADIO_BRIDGE_UART_RING_SIZE=16384
	CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US=1000
	CONFIG_RADIO_BRIDGE_SYNC_INTERVAL_US=100000
	CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_COUNT=8
	CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_US=500
	CONFIG_RADIO_BRIDGE_ASSIGNMENT_WINDOW_US=6000
	CONFIG_RADIO_BRIDGE_LEASE_TIMEOUT_US=100000
	CONFIG_RADIO_BRIDGE_IDLE_POLL_MAX_US=5000
	CONFIG_TIME_SYNC_PPS_PERIOD_US=1000000
	CONFIG_TIME_SYNC_PPS_WIDTH_US=100000
	CONFIG_TIME_SYNC_RADIO_DELAY_US=0
	CONFIG_TIME_SYNC_STATUS_INTERVAL_MS=1000
	CONFIG_TIME_UART_BAUDRATE=9600
)
target_sources(app PRIVATE
	src/test_config.c
	src/test_param_config.c
	../../../src/config/bridge_config.c
	../../../src/config/param_config.c
)
```

Create `tests/unit/radio/CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radio_tests)

target_include_directories(app PRIVATE
	../../../src/bridge
	../../../src/radio
)
target_sources(app PRIVATE
	src/test_radio_address.c
	../../../src/radio/radio_address.c
)
```

Both `prj.conf` files contain:

```conf
CONFIG_ZTEST=y
CONFIG_PRINTK=y
```

Update all unit CMake relative paths from `../../src` to `../../../src/<module>` and remove moved tests/sources from the bridge suite.

- [ ] **Step 4: Split and move host tests**

Create `tests/host/support.py` containing the existing `load_board_e2e()` and `require_symbol()` helpers. Move classes without changing their assertions:

```text
tests/host/test_board_session.py:
  ResolveSerialTests, CommandRecoveryTests, HistoryCursorTests,
  ShellOutputTests, RuntimeFreshnessTests, BootGuardClearTests,
  TimeUartFreshnessTests, ReadinessFreshnessTests, RoleManagementTests

tests/host/test_board_runner.py:
  RecoveryFlashTests, SteadyStateTests, RawLogTests,
  StageResultTests, RunnerEvidenceTests, CliTests
```

Move `tests/test_flash_uf2.py` to `tests/host/test_flash_uf2.py`; add `tests/host/__init__.py`; delete the emptied `tests/test_board_e2e.py`.

- [ ] **Step 5: Run reorganized suites, firmware builds and stage-2 hardware gate**

```bash
for suite in bridge config radio time time_uart; do
  ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native "tests/unit/${suite}" -d "build/tests/${suite}"
  "./build/tests/${suite}/${suite}/zephyr/zephyr.exe"
done
/home/hv/ncs/.venv/bin/python -m unittest discover -v -s tests/host -t .
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
./build.sh master -d build/cleanup-stage2-production
./build.sh master -d build/cleanup-stage2-validation -- -DOVERLAY_CONFIG=validation.conf
/usr/bin/timeout 240s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage cleanup-stage2-source-layout \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/cleanup-stage2-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/cleanup-stage2-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --steady-state-seconds 5 --command-timeout 15
```

Expected: every suite/build exits 0 and board gate passes with zero retry/recovery/drop/error.

- [ ] **Step 6: Commit stage 2**

```bash
git diff --check
git add CMakeLists.txt src tests
git commit -m "refactor: organize source and tests by function"
```

### Task 3: Reduce NVS parameters and audit Kconfig

**Files:**
- Modify: `src/config/param_defs.h`
- Modify: `src/config/param_config.c`
- Modify: `src/config/app_config.h`
- Modify: `src/bridge/bridge_runtime.c`
- Modify: `src/app/main.c`
- Modify: `src/app/heartbeat_led.c`
- Modify: `src/app/heartbeat_led.h`
- Modify: `Kconfig`
- Modify: `prj.conf`
- Modify: unit-test CMake compile definitions
- Modify: `tests/unit/config/src/test_param_config.c`
- Create: `docs/verification/kconfig-audit.md`

- [ ] **Step 1: Write the failing eight-parameter table test**

Add to `tests/unit/config/src/test_param_config.c`:

```c
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
```

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native tests/unit/config -d build/tests/config
./build/tests/config/config/zephyr/zephyr.exe
```

Expected: FAIL because the current table has 21 entries and legacy IDs.

- [ ] **Step 2: Compact the parameter table**

Use this enum order in `src/config/param_defs.h`:

```c
enum rb_param_id {
	RB_PARAM_ROLE_ID = 0,
	RB_PARAM_GROUP_ID,
	RB_PARAM_GROUP_KEY,
	RB_PARAM_UART_BAUDRATE,
	RB_PARAM_TIME_SOURCE_MODE,
	RB_PARAM_TIME_UART_BAUDRATE,
	RB_PARAM_PPS_INPUT_DELAY_US,
	RB_PARAM_RADIO_DELAY_US,
	RB_PARAM_COUNT,
};
```

Keep only matching descriptors in `src/config/param_config.c`, in enum order, with contiguous NVS IDs `0x1000..0x1007`. Preserve current ranges/defaults: role `0..3/default 0`, group `1..0xfffffffe`, 16-byte group key, data UART `1200..3000000`, time source `0..1/default 0`, time UART `1200..115200`, PPS/radio delays `0..1000/default 0`.

Update existing parameter tests so reset/reboot/default assertions reference only retained IDs. Delete assertions for aggregation, ring, PPS width, status and LED parameters.

- [ ] **Step 3: Replace removed NVS consumers with build values**

In `src/config/app_config.h`, define:

```c
#define RADIO_BRIDGE_MAX_SLAVES_VALUE 3u
#define TIME_SYNC_PPS_PERIOD_US_VALUE 1000000u
#define TIME_SYNC_STATUS_INTERVAL_MS_VALUE CONFIG_TIME_SYNC_STATUS_INTERVAL_MS
```

Keep ring, aggregation, sync/slot/window/lease, PPS width and LED mappings from their remaining Kconfig symbols.

In `src/bridge/bridge_runtime.c`, delete parameter reads for removed IDs and initialize scheduler fields directly from:

```c
CONFIG_RADIO_BRIDGE_SYNC_INTERVAL_US
CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US
CONFIG_RADIO_BRIDGE_IDLE_POLL_MAX_US
CONFIG_RADIO_BRIDGE_LEASE_TIMEOUT_US
CONFIG_RADIO_BRIDGE_ASSIGNMENT_WINDOW_US
CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_COUNT
CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_US
```

In `src/app/main.c`, use:

```c
const uint32_t status_interval_ms = CONFIG_TIME_SYNC_STATUS_INTERVAL_MS;

ret = pps_output_init(CONFIG_TIME_SYNC_PPS_WIDTH_US);
ret = heartbeat_led_start(IS_ENABLED(CONFIG_TIME_SYNC_LED_HEARTBEAT),
			  CONFIG_TIME_SYNC_LED_HEARTBEAT_PERIOD_MS);
```

Remove per-loop parameter reload and `heartbeat_led_update()`. If no caller remains, delete `heartbeat_led_update()` from `heartbeat_led.c/.h`.

- [ ] **Step 4: Audit every application Kconfig symbol**

Create `docs/verification/kconfig-audit.md` with `Symbol | Consumers | Decision | Reason` and one row per symbol in root `Kconfig`.

Delete these proven low-value symbols and corresponding `prj.conf` entries:

```text
RADIO_BRIDGE_VALIDATION_STAGE  no source consumer
RADIO_BRIDGE_NEW_STACK        legacy selector; only ESB remains
RADIO_BRIDGE_MAX_SLAVES       range fixed to 3..3
TIME_SYNC_PPS_PERIOD_US       range fixed to one second
```

Retain documented build-time controls for group defaults, UART defaults, ring capacity, aggregation/sync/slot/window/lease timings, PPS width/radio delay, log/LED diagnostics, validation CDC and loss injection. These affect compiled defaults, memory, protocol timing, hardware output or explicit test variants.

- [ ] **Step 5: Run parameter test green and full native/host regression**

```bash
for suite in bridge config radio time time_uart; do
  ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native "tests/unit/${suite}" -d "build/tests/${suite}"
  "./build/tests/${suite}/${suite}/zephyr/zephyr.exe"
done
/home/hv/ncs/.venv/bin/python -m unittest discover -v -s tests/host -t .
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
```

Expected: the eight-parameter test and every existing design test pass.

- [ ] **Step 6: Clear the old NVS layout before stage-3 flash**

With stage-2 validation firmware still running, send `param reset` to both exact CDC paths, wait for the success response, then issue `kernel reboot cold`. Save timestamped transcripts in `build/board-e2e/cleanup-stage3-preflash-nvs-reset/`.

Before flashing stage 3, verify on both boards:

```text
param get role_id
param get group_id
param get group_key
```

Expected: default role `0`, group `1`, default key, and no persisted marker.

- [ ] **Step 7: Build and run stage-3 parameter/hardware gates**

```bash
./build.sh master -d build/cleanup-stage3-production
./build.sh master -d build/cleanup-stage3-validation -- -DOVERLAY_CONFIG=validation.conf
/usr/bin/timeout 300s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage cleanup-stage3-params-kconfig \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/cleanup-stage3-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/cleanup-stage3-validation/zephyr/zephyr.uf2 \
  --cycles 1 --cold-reboots-per-board 1 --bridge-length 600 \
  --steady-state-seconds 5 --command-timeout 15
```

After the gate, run `param list` on both boards and require exactly eight names. Set slave `group_id=2`, cold reboot and require master `active_count=0` plus slave `UNLOCKED`; restore `group_id=1` and rerun one 600-byte bidirectional gate.

- [ ] **Step 8: Commit stage 3**

```bash
git diff --check
git add Kconfig prj.conf src tests docs/verification/kconfig-audit.md
git commit -m "refactor: simplify runtime and build configuration"
```

### Task 4: Rewrite README and perform final acceptance

**Files:**
- Replace: `README.md`
- Modify: `docs/verification/incremental-board-e2e-results.md`

- [ ] **Step 1: Rewrite README with exactly two level-2 sections**

Keep the project title. The only level-2 headings are:

```markdown
## 快速开始
## 工程介绍
```

`快速开始` includes NCS dependencies, production/validation builds, fixed-ID UF2 flash, pin table, eight parameters, master/slave and local/external configuration, plus the standard dual-board gate.

`工程介绍` includes topology, broadcast downlink, independent reliable slave records, 1 Hz/100 ms PPS, local/external master time source, NVS fallback, group isolation/security boundary, functional source tree, production/validation boundary and external GNSS/oscilloscope acceptance.

Use these canonical commands:

```bash
./build.sh master -d build/production
./build.sh master -d build/validation -- -DOVERLAY_CONFIG=validation.conf
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py --list
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage local-check \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600
```

- [ ] **Step 2: Verify README paths and structure**

```bash
rg -n '^## ' README.md
rg -n 'tests/(bridge_logic|sync_logic|time_uart_logic)|tests/\*_static_check|src/[a-z_]+\.c' README.md
```

Expected: exactly two `##` lines; second command has no matches.

- [ ] **Step 3: Run final host/native/build verification**

```bash
for suite in bridge config radio time time_uart; do
  ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native "tests/unit/${suite}" -d "build/tests/${suite}"
  "./build/tests/${suite}/${suite}/zephyr/zephyr.exe"
done
/home/hv/ncs/.venv/bin/python -m unittest discover -v -s tests/host -t .
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
./build.sh master -d build/cleanup-final-production
./build.sh master -d build/cleanup-final-validation -- -DOVERLAY_CONFIG=validation.conf
git diff --check
```

Expected: every command exits 0.

- [ ] **Step 4: Run final board and physical-UART acceptance**

```bash
/usr/bin/timeout 300s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage cleanup-final \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/cleanup-final-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/cleanup-final-validation/zephyr/zephyr.uf2 \
  --cycles 1 --cold-reboots-per-board 1 --bridge-length 600 \
  --steady-state-seconds 10 --command-timeout 15
```

Then flash `build/cleanup-final-production/zephyr/zephyr.uf2` to both boards. On `/dev/ttyACM2` ↔ master and adb `/dev/ttyS1` ↔ slave at 115200 8N1, send 32-byte frames at 10 pps and 64-byte frames at 100 pps in both directions. Require exact framed SHA/CRC, zero board drops, and latency min/P50/P95/P99/max with calibrated uncertainty.

- [ ] **Step 5: Record and commit final stage**

Append only final build hashes, gate evidence and physical-UART smoke summary to `docs/verification/incremental-board-e2e-results.md`.

```bash
git diff --check
git add README.md docs/verification/incremental-board-e2e-results.md
git commit -m "docs: streamline project guide after cleanup"
git push origin validation/incremental-board-e2e
```

Expected: clean worktree and matching local/remote final SHA.
