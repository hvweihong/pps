# Incremental Board E2E Validation Implementation Plan

> **For agentic workers:** Execute this plan task-by-task in the current workspace. Do not create a worktree. Do not commit later changes unless the user explicitly requests it.

**Goal:** Starting from `main`, prove both fixed USB-identified XIAO boards can run the complete ESB UART bridge and wireless time-sync stack, using staged hardware gates that isolate cold-boot failures.

**Architecture:** Keep the existing `main` BLE/MPSL application as validation stage 0–4, and add a Kconfig-selected stage controller. At stage 5, CMake switches from the old BLE/MPSL source set to the ESB bridge source set; later stages enable discovery, scheduling, wireless sync/PPS re-lock, runtime parameters, and validation-only CDC injection commands. Every stage has host tests, a pristine build, fixed-ID flashing, and single-board reset cycles before the next stage.

**Tech Stack:** nRF Connect SDK v3.3.1, Zephyr, nRF52840, C, Kconfig/CMake, native_sim/ztest, Python 3 + unittest + pyserial, UF2 bootloader, USB CDC ACM.

**Workspace rules:** Work only in `/home/hv/projects/pps` on `validation/incremental-board-e2e`. The prior feature-branch changes are preserved in commit `6fb76cd`; do not switch back to that branch during implementation. The external hub is ganged-power, so whole-hub power cycling is a joint two-board reset only.

---

### Task 1: Capture the clean `main` baseline

**Files:**
- Read-only: `build.sh`, `master.conf`, `slave.conf`, `prj.conf`, `Kconfig`, `CMakeLists.txt`, `src/main.c`
- Read-only: `tests/sync_logic/`, `tests/*_static_check.sh`

- [ ] **Step 1: Confirm branch and clean baseline metadata**

Run:

```bash
git status --short --branch
git log -1 --oneline --decorate
git merge-base --is-ancestor main HEAD
```

Expected: branch is `validation/incremental-board-e2e`, `HEAD` is based on `main`, and no tracked files are modified.

- [ ] **Step 2: Run the existing native sync test**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
tests/sync_logic -d build/tests/main_sync_logic
./build/tests/main_sync_logic/sync_logic/zephyr/zephyr.exe
```

Expected: the ztest executable exits zero with no failed test cases.

- [ ] **Step 3: Run all `main` static checks and both pristine builds**

Run:

```bash
for f in tests/*_static_check.sh; do bash "$f"; done
./build.sh master
./build.sh slave
```

Expected: every existing static check passes; both builds generate `zephyr.elf`, `zephyr.hex`, and `zephyr.uf2` without warnings treated as errors. Record `.config` values, image sizes, and the exact `HEAD` in `docs/verification/incremental-board-e2e-results.md`.

---

### Task 2: Add recovery tooling and the stage controller

**Files:**
- Create: `tools/__init__.py`
- Create: `tools/flash_uf2.py` (copy the complete implementation from `feature/star-radio-uart-bridge-main`)
- Create: `tests/test_flash_uf2.py` (copy the complete seven-test suite from the feature branch)
- Modify: `Kconfig`
- Modify: `prj.conf`
- Modify: `src/main.c`
- Modify: `CMakeLists.txt`
- Create: `tests/boot_recovery_static_check.sh`

- [ ] **Step 1: Add failing recovery checks before implementation**

Create `tests/boot_recovery_static_check.sh` with these exact assertions:

```bash
#!/usr/bin/env bash
set -euo pipefail
rg -q '^CONFIG_WATCHDOG=y$' prj.conf
rg -q '^CONFIG_WDT_NRFX=y$' prj.conf
rg -q 'DFU_MAGIC_UF2_RESET' src/main.c
rg -q 'usbd_msg_register_cb' src/main.c
rg -q 'wdt_feed' src/main.c
```

Run `chmod +x tests/boot_recovery_static_check.sh && bash tests/boot_recovery_static_check.sh` and verify it fails because the recovery symbols are not present on `main`.

- [ ] **Step 2: Copy and test serial-bound UF2 discovery**

Copy `tools/flash_uf2.py`, `tools/__init__.py`, and `tests/test_flash_uf2.py` from the committed feature branch without changing their serial-ID matching rules. Run:

```bash
/home/hv/ncs/.venv/bin/python -m unittest -v tests/test_flash_uf2.py
```

Expected: all seven tests pass, including exact-ID matching and fail-closed ambiguity handling.

- [ ] **Step 3: Add the watchdog configuration**

Append these lines to `prj.conf`:

```text
CONFIG_WATCHDOG=y
CONFIG_WDT_NRFX=y
CONFIG_REBOOT=y
CONFIG_UART_LINE_CTRL=y
```

- [ ] **Step 4: Add watchdog initialization and feeding**

In `src/main.c`, add `<zephyr/drivers/watchdog.h>`, define a static watchdog device and channel, and use the same 30-second `wdt_timeout_cfg` as the preserved feature implementation. Call `watchdog_init()` before `timebase_init()`. Feed the channel once per iteration in both `master_loop()` and `slave_loop()` immediately before their existing sleep calls. If setup fails, log the error and continue so the baseline exposes the failure.

- [ ] **Step 5: Add the 1200-baud UF2 callback**

In `src/main.c`, add the USB message, UART line-control, nRF power, reboot, and section-iteration includes required by this Zephyr version. Add the callback behavior:

```c
#define DFU_MAGIC_UF2_RESET 0x57u
static void usbd_line_coding_cb(struct usbd_context *const ctx,
                                const struct usbd_msg *const msg)
{
        uint32_t rate = 0u;
        ARG_UNUSED(ctx);
        if (msg->type != USBD_MSG_CDC_ACM_LINE_CODING ||
            uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_BAUD_RATE, &rate) != 0 ||
            rate != 1200u) {
                return;
        }
        nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC_UF2_RESET);
        sys_reboot(SYS_REBOOT_COLD);
}
```

Register it for every `usbd_context` before entering either role loop:

```c
STRUCT_SECTION_FOREACH(usbd_context, usbd_ctx) {
        (void)usbd_msg_register_cb(usbd_ctx, usbd_line_coding_cb);
}
```

- [ ] **Step 6: Add the integer validation stage symbol**

Add this Kconfig symbol:

```text
config RADIO_BRIDGE_VALIDATION_STAGE
        int "Incremental board validation stage"
        default 0
        range 0 8
```

Use `CONFIG_RADIO_BRIDGE_VALIDATION_STAGE` in CMake and C preprocessor guards; stage 0 must compile and run the existing BLE/MPSL source set.

- [ ] **Step 7: Verify recovery changes**

Run `bash tests/boot_recovery_static_check.sh`, `./build.sh master`, and `./build.sh slave`. Expected: the new static check passes and both role images still build with BLE/MPSL enabled.

---

### Task 3: Establish the `main` two-board hardware gate

**Files:**
- Create: `docs/verification/incremental-board-e2e-results.md`
- Read-only hardware: `DBE5C3D84EA2EC6F` (master)
- Read-only hardware: `5B3D71D27A709CA2` (slave)

- [ ] **Step 1: Verify fixed-ID discovery**

Run `/home/hv/ncs/.venv/bin/python tools/flash_uf2.py --list`. Expected: both IDs report `application`; never select a board by `ttyACM` number.

- [ ] **Step 2: Flash only the master image**

```bash
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py \
  --device-id DBE5C3D84EA2EC6F --role master \
  --uf2 build/master/zephyr/zephyr.uf2 --repeat 5 --retry 1
```

Expected: five complete 1200-baud→UF2→application cycles and the exact master ID reappears after each cycle.

- [ ] **Step 3: Flash only the slave image**

Run the same command with `5B3D71D27A709CA2`, `--role slave`, and `build/slave/zephyr/zephyr.uf2`. Expected: five complete cycles; the master remains untouched.

- [ ] **Step 4: Check baseline logs and reset behavior**

Capture at least 10 seconds from each fixed-ID CDC path at 115200 baud. Require `firmware:`, `role=`, `sync_status`, and `pps_status` lines as appropriate. Issue `kernel reboot cold` to one board at a time and confirm it re-enumerates while the other remains present.

- [ ] **Step 5: Complete the baseline stability gate**

Perform 30 one-board `kernel reboot cold` cycles per role. Record every missing enumeration, watchdog reset, shell timeout, and sync/PPS counter reset. Proceed only with zero unexplained failures.

---
### Task 4: Migrate NVS parameters without changing the radio backend

Files:
- Modify: Kconfig, prj.conf, CMakeLists.txt, src/main.c
- Create: src/bridge_config.[ch], src/param_defs.h, src/param_config.[ch], src/param_shell.c
- Create: tests/bridge_logic/src/test_config.c and tests/bridge_logic/src/test_param_config.c

- [ ] Step 1: Add parameter native tests first

Copy the feature parameter tests and native test CMake/prj files. Build before adding production files and confirm the expected missing-symbol failure.

- [ ] Step 2: Add settings/NVS dependencies

Add these exact symbols to prj.conf:

~~~text
CONFIG_SETTINGS=y
CONFIG_SETTINGS_RUNTIME=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_MPU_ALLOW_FLASH_WRITE=y
CONFIG_NVS=y
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=y
CONFIG_SHELL_PROMPT_UART="uart:~$ "
CONFIG_SHELL_CMD_BUFF_SIZE=128
CONFIG_SHELL_PRINTF_BUFF_SIZE=128
~~~

Add feature Kconfig definitions for group ID/key, UART baud/ring, sync interval, scheduler timings, PPS, status, and LED values, while retaining compile-time BLE/MPSL role selection.

- [ ] Step 3: Wire the parameter implementation

Copy the complete feature implementations of bridge_config.[ch], param_defs.h, param_config.[ch], and param_shell.c; add them to CMakeLists.txt. Call rb_param_config_init() before the existing runtime_config_init() and log a hard error if settings initialization fails.

- [ ] Step 4: Verify persistence and bounds

Run the native parameter suite and static checks. On one board issue param set status_interval_ms 2000, param get status_interval_ms, and kernel reboot cold; require the value to remain 2000. Test one below-range and one above-range value and require a shell error with no changed value.

---

### Task 5: Enable and verify the UART bridge hardware layer

Files:
- Modify: boards/xiao_ble_nrf52840.overlay, prj.conf, Kconfig, CMakeLists.txt, src/main.c
- Create: src/byte_ring.[ch], src/uart_bridge.[ch], tests/uart_bridge_static_check.sh

- [ ] Step 1: Add the failing UART static check

Copy the feature UART static check and run it before implementation. It must fail until the async UART symbols and bridge-uart = &uart0 alias exist.

- [ ] Step 2: Enable only UART driver requirements

Add:

~~~text
CONFIG_UART_ASYNC_API=y
CONFIG_UART_0_INTERRUPT_DRIVEN=n
CONFIG_UART_0_ASYNC=y
CONFIG_UART_USE_RUNTIME_CONFIGURE=y
CONFIG_RADIO_BRIDGE_UART_BAUDRATE=115200
CONFIG_RADIO_BRIDGE_UART_RING_SIZE=16384
CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US=1000
~~~

- [ ] Step 3: Add the ring and async bridge

Copy feature byte_ring.[ch] and uart_bridge.[ch], add them to CMake, and add bridge-uart = &uart0 to the overlay. Keep the old BLE/MPSL data path unchanged; call only uart_bridge_init() from main() after PPS initialization and log its return code.

- [ ] Step 4: Verify board-side UART initialization

Build both roles, flash one fixed-ID board at a time, and capture 10 seconds of logs. Require successful UART configure/callback/RX-enable diagnostics, no rx_restart_errors, no TX aborts, and zero idle drops. Repeat five UF2 cycles and 30 kernel reboot cold cycles per board.

---

### Task 6: Switch to fixed-role ESB transport with no dynamic profile

Files:
- Modify: Kconfig, prj.conf, CMakeLists.txt, boards/xiao_ble_nrf52840.overlay, src/main.c
- Create: src/radio_address.[ch], src/radio_address_crypto.[ch], src/radio_transport.[ch], src/bridge_stage_main.c, tests/radio_transport_static_check.sh
- Create: validation/stage5-master.conf and validation/stage5-slave.conf

- [ ] Step 1: Add stage-5 CMake guards

Keep existing BLE/MPSL sources for stages below 5. Add ESB/address/transport sources plus src/bridge_stage_main.c at stage 5 or higher. Compile the existing main() only when CONFIG_RADIO_BRIDGE_VALIDATION_STAGE < 5.

- [ ] Step 2: Add ESB configuration

Add:

~~~text
CONFIG_RADIO_BRIDGE_NEW_STACK=y
CONFIG_ESB=y
CONFIG_ESB_MAX_PAYLOAD_LENGTH=252
CONFIG_ESB_PIPE_COUNT=4
CONFIG_ESB_SYS_TIMER1=y
CONFIG_ESB_DYNAMIC_INTERRUPTS=y
CONFIG_ESB_MPSL_TIMESLOT=n
CONFIG_CRYPTO=y
CONFIG_CRYPTO_NRF_ECB=y
CONFIG_HWINFO=y
CONFIG_NRFX_GPPI=y
CONFIG_MPSL=y
CONFIG_RADIO_BRIDGE_VALIDATION_STAGE=5
~~~

Create stage-5 master/slave overlays with the same group ID/key and opposite fixed-role symbols.

- [ ] Step 3: Implement the fixed ESB stage main

src/bridge_stage_main.c initializes timebase, PPS, UART, derived group addresses, and radio_transport_init(), then runs a minimal status loop. It must not call discovery, esb_disable() profile switching, or the scheduler. Add stage markers immediately before and after esb_init() and esb_start_rx().

- [ ] Step 4: Verify the first RADIO boundary

Run native/static tests, build stage-5 images, and flash one fixed-ID board at a time. Require 30 cold-reset cycles per board, exact USB re-enumeration, no watchdog reset, no ESB stage timeout, and increasing fixed-profile TX/RX counters.

---

### Task 7: Enable discovery, ASSIGN, membership, scheduler, and profiles

Files:
- Modify: Kconfig, prj.conf, CMakeLists.txt, src/bridge_stage_main.c
- Create: src/link_protocol.[ch], src/link_window.[ch], src/membership.[ch], src/link_scheduler_core.[ch], src/sync_tracker.[ch]
- Create: feature bridge protocol/window/membership/scheduler native tests

- [ ] Step 1: Add native tests before implementation

Copy feature suites for link protocol, window, membership, master scheduler, slave scheduler, sync tracker, and byte-ring behavior. Run the stage build and confirm missing-symbol failures before adding production sources.

- [ ] Step 2: Add scheduler defaults

~~~text
CONFIG_RADIO_BRIDGE_MAX_SLAVES=3
CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_COUNT=8
CONFIG_RADIO_BRIDGE_RESPONSE_SLOT_US=500
CONFIG_RADIO_BRIDGE_ASSIGNMENT_WINDOW_US=6000
CONFIG_RADIO_BRIDGE_LEASE_TIMEOUT_US=100000
CONFIG_RADIO_BRIDGE_IDLE_POLL_MAX_US=5000
~~~

- [ ] Step 3: Migrate the tested scheduler

Copy feature protocol, window, membership, scheduler, and tracker sources. Preserve fixes for ASSIGN response-window waiting, hardware TX completion before profile changes, and DROP_OLDEST accounting.

- [ ] Step 4: Prove one-slave admission

Set stage 6 with the fixed-role master/slave validation overlays, and run the full native bridge suite. Hardware must show discovery_hello_count > 0, active_count >= 1, no suspect growth while both boards are alive, and successful slave re-admission after restarting only the slave. Runtime role IDs are enabled only in Task 8. Run 30 cold-reset cycles per board.

---

### Task 8: Add ESB wireless sync, PPS re-lock, and runtime integration

Files:
- Modify: src/bridge_stage_main.c, Kconfig, prj.conf, CMakeLists.txt, build.sh
- Create: src/wireless_time_sync.[ch], src/bridge_runtime.[ch], src/time_sync_shell.c
- Modify: src/sync_filter.[ch], src/time_sync_math.c, src/status.[ch]
- Create: feature sync/native regression tests

- [ ] Step 1: Add sync tests before runtime wiring

Copy and run feature sync logic and wireless-time-sync suites before adding runtime calls; confirm missing bridge-sync symbols fail as expected.

- [ ] Step 2: Add sync/PPS stage configuration

~~~text
CONFIG_RADIO_BRIDGE_SYNC_INTERVAL_US=100000
CONFIG_TIME_SYNC_PPS_PERIOD_US=1000000
CONFIG_TIME_SYNC_PPS_WIDTH_US=100
CONFIG_TIME_SYNC_STATUS_INTERVAL_MS=1000
~~~

Use a boolean CONFIG_RADIO_BRIDGE_ENABLE_SYNC that is off through stage 6 and on at stage 7; never call wireless_time_sync_slave_receive() while it is off.

- [ ] Step 3: Wire bridge runtime in the safe order

Preserve feature order: parameters/address/filter/UART, then radio_transport_init(), then UART/radio wake callbacks, USB line-coding callback, and bridge thread. Preserve successful-sync-TX capture publication and TX-failure clearing.

- [ ] Step 4: Verify wireless lock and PPS software health

Build stage 7, flash both fixed IDs, and read bridge_radio, bridge_sync, and bridge_pps. Require increasing master TX/slave RX, slave locked state, missed not exceeding received, bounded offset/jitter, increasing PPS count, and slave restart followed by discovery and re-lock. Do not claim phase accuracy without an oscilloscope.

---

### Task 9: Add validation-only CDC injection and host monitors

Files:
- Create: src/validation_shell.c, tools/cdc_monitor.py, tools/uart_bridge_stress.py
- Modify: src/bridge_runtime.[ch], Kconfig, CMakeLists.txt
- Create: tests/test_cdc_status.py, tests/test_cdc_integration.py, tests/uart_bridge_static_check.sh

- [ ] Step 1: Add parser tests before hardware I/O

Copy feature CDC parser/integration tests and run:

~~~bash
/home/hv/ncs/.venv/bin/python -m unittest -v tests/test_cdc_status.py tests/test_cdc_integration.py
~~~

Expected: parser/enumeration tests pass; hardware tests skip only when expected stage status prefixes are absent.

- [ ] Step 2: Define validation-only commands

Under CONFIG_RADIO_BRIDGE_VALIDATION_DIAGNOSTICS, implement:

~~~text
bridge_test inject downlink <hex>
bridge_test inject uplink <hex>
bridge_test drain
bridge_test uart_tx <hex>
~~~

The production default is n; commands must not change normal wire framing or status counters.

- [ ] Step 3: Verify the complete USB-only data path

With no physical UART adapter, inject and drain exact payloads of 1, 230, 252, 4096, and 16384 bytes. Enable controlled loss injection for a repair test; require eventual payload equality and nonzero repair counters.

---

### Task 10: Run final full-stack acceptance and publish evidence

Files:
- Modify: docs/verification/incremental-board-e2e-results.md
- Modify: README.md only if command/configuration behavior changed

- [ ] Step 1: Run the complete stage-8 software gate

Run the bridge native suite, sync native suite, every tests/*_static_check.sh, both Python parser suites, and ./build.sh. Expected: all non-hardware tests pass; absent external peripherals are explicitly skipped.

- [ ] Step 2: Flash and capture the full image

Flash the unified stage-8 UF2 to each fixed ID with --repeat 5 --retry 1, keep both boards in application mode, and capture 60 seconds of status from each.

- [ ] Step 3: Run final per-board reset statistics

Perform 50 kernel reboot cold cycles per board, alternating master and slave. Require zero unexplained USB disappearance, watchdog reset, shell loss, failed re-admission, failed sync re-lock, or PPS counter stall.

- [ ] Step 4: Run the joint hub-power test when available

If the user can turn the external hub off/on, perform 10 joint cycles. After each cycle require both fixed IDs, master active operation, slave discovery/lock, and both PPS counters. Record this only as joint-reset evidence.

- [ ] Step 5: Record explicit coverage limits

Document physical D6/D7 UART I/O, PPS phase error, and three-slave/fourth-candidate capacity as unmeasured without external hardware. For every failure retain stage marker, reset reason, exact cycle, logs, and the smallest reproducing configuration.

---

## Plan self-review

- Every design stage has a corresponding task and hardware gate.
- The old BLE/MPSL source set is retained through stage 4 and excluded at stage 5.
- UART initialization is tested independently from physical UART traffic; CDC diagnostics cover the USB-only wireless path without claiming external-pin validation.
- Whole-hub power is explicitly treated as a joint reset.
- No step depends on ttyACM number or enumeration order.
- No post-baseline commit is required; later changes remain in this validation branch until the user requests integration.
