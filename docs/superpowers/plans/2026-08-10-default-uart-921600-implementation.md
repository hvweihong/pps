# Default Data UART 921600 Baud Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make 921600 baud the compiled fallback for the physical data UART and prove the change through native, dual-board CDC/wireless, and physical-UART tests.

**Architecture:** Keep `uart_baudrate` as the single NVS override and change only its Kconfig fallback. Preserve USB CDC and external time-UART settings. Clear the two boards' old persisted data-UART values before hardware validation so the test exercises the new fallback rather than a legacy NVS value.

**Tech Stack:** Zephyr/NCS 3.3.1, C/Kconfig, Ztest native_sim, Python/pyserial, dual XIAO nRF52840 Plus, UF2/CDC, adb `/dev/ttyS1`.

---

### Task 1: Change the compiled data-UART default with TDD

**Files:**
- Modify: `tests/unit/config/src/test_param_config.c`
- Modify: `tests/unit/config/CMakeLists.txt`
- Modify: `Kconfig`
- Modify: `README.md`

- [ ] **Step 1: Write the failing default-value assertion**

In `test_defaults_used_when_settings_empty`, change only the compiled-default assertion:

```c
zassert_equal(value, 921600, "Should return compiled UART default");
```

Keep `CONFIG_RADIO_BRIDGE_UART_BAUDRATE=115200` temporarily so the test proves the old default is still active. The other 115200 set/get assertions remain unchanged because 115200 stays a valid persisted value.

- [ ] **Step 2: Run the config suite and verify RED**

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build \
  -p always -b native_sim/native tests/unit/config -d build/tests/config
./build/tests/config/config/zephyr/zephyr.exe
```

Expected: the suite fails at `Should return compiled UART default`, reporting actual 115200 versus expected 921600.

- [ ] **Step 3: Apply the minimal implementation**

Change root `Kconfig`:

```kconfig
config RADIO_BRIDGE_UART_BAUDRATE
	int "UART baud rate"
	default 921600
	range 1200 3000000
```

Change `tests/unit/config/CMakeLists.txt` to:

```cmake
CONFIG_RADIO_BRIDGE_UART_BAUDRATE=921600
```

Update the reset/default assertion in `test_clear_restores_default` to 921600. Do not change tests which explicitly persist 115200.

In README, change only the physical data-UART pin-table default and the `uart_baudrate` compiled-default table cell from 115200 to 921600. Keep the USB CDC 115200 line-coding and time-UART 9600/115200 values unchanged.

- [ ] **Step 4: Run the config suite and verify GREEN**

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build \
  -p always -b native_sim/native tests/unit/config -d build/tests/config
./build/tests/config/config/zephyr/zephyr.exe
```

Expected: `PROJECT EXECUTION SUCCESSFUL`.

- [ ] **Step 5: Verify generated production configuration and focused diff**

```bash
./build.sh master -d build/uart-921600-production
rg '^CONFIG_RADIO_BRIDGE_UART_BAUDRATE=921600$' \
  build/uart-921600-production/zephyr/.config
git diff --check
```

Expected: build exits 0, the generated value is exactly 921600, and the diff check is clean.

- [ ] **Step 6: Commit the default-value stage**

```bash
git add Kconfig README.md tests/unit/config/CMakeLists.txt \
  tests/unit/config/src/test_param_config.c
git commit -m "feat: default data UART to 921600 baud"
```

### Task 2: Run full software and dual-board validation

**Files:**
- Build artifacts only under `build/`

- [ ] **Step 1: Run every software regression**

```bash
for suite in bridge config radio time time_uart; do
  ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build \
    -p always -b native_sim/native "tests/unit/${suite}" -d "build/tests/${suite}"
  "./build/tests/${suite}/${suite}/zephyr/zephyr.exe"
done
/home/hv/ncs/.venv/bin/python -m unittest discover -v -s tests/host -t .
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
./build.sh master -d build/uart-921600-production
./build.sh master -d build/uart-921600-validation -- \
  -DOVERLAY_CONFIG=validation.conf
```

Expected: five native suites, 69 host tests, ruff, and both firmware builds exit 0.

- [ ] **Step 2: Flash validation firmware and clear legacy UART values**

Use `tools.board_e2e.BoardSession` with the exact IDs and timestamped raw logs to flash
`build/uart-921600-validation/zephyr/zephyr.uf2` to each board, then issue:

```text
param clear uart_baudrate
kernel reboot cold
param get uart_baudrate
```

Preserve `role_id=0` on `DBE5C3D84EA2EC6F` and `role_id=1` on `5B3D71D27A709CA2`. Expected on both boards:

```text
uart_baudrate = 921600 (default, reboot)
```

Save logs under `build/board-e2e/<timestamp>-uart-921600-nvs-clear/`.

- [ ] **Step 3: Run the standard final hardware gate**

```bash
/usr/bin/timeout 300s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage uart-921600-final \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/uart-921600-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/uart-921600-validation/zephyr/zephyr.uf2 \
  --cycles 1 --cold-reboots-per-board 1 --bridge-length 600 \
  --steady-state-seconds 10 --command-timeout 15
```

Expected: slave `LOCKED`, both 600-byte directions exact before and after cold reboot, and zero flash retry, recovery, queue drop, input error, and UART start error.

### Task 3: Validate the production physical UART at 921600

**Files:**
- Create ignored evidence fixture: `build/final-verification/physical_uart_921600.py`
- Modify: `docs/verification/incremental-board-e2e-results.md`

- [ ] **Step 1: Flash production firmware without changing NVS**

```bash
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py \
  --device-id DBE5C3D84EA2EC6F \
  --uf2 build/uart-921600-production/zephyr/zephyr.uf2
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py \
  --device-id 5B3D71D27A709CA2 \
  --uf2 build/uart-921600-production/zephyr/zephyr.uf2
```

Expected: both exact-ID flashes succeed on the first attempt and the preserved roles relock.

- [ ] **Step 2: Create a fail-closed physical fixture in the ignored build directory**

The fixture must:

- configure local `/dev/ttyACM2` and adb `/dev/ttyS1` as 921600 8N1, raw, no flow control;
- frame each record with magic, direction, sequence, payload length and CRC32;
- run `32 B × 10 pps × 20` and `64 B × 100 pps × 100` in both directions;
- compare every sequence/length/CRC plus concatenated expected/received SHA-256;
- timestamp source first-byte send and destination complete-frame receive;
- calibrate adb remote monotonic time using minimum-RTT midpoint samples;
- report latency min/P50/P95/P99/max and calibration uncertainty;
- time out and return nonzero on missing, extra, reordered or corrupt data; never fall back to 115200.

This file stays under ignored `build/` evidence and is not added to the source tree.

- [ ] **Step 3: Run the 921600 physical matrix**

```bash
/home/hv/ncs/.venv/bin/python \
  build/final-verification/physical_uart_921600.py \
  --local-port /dev/ttyACM2 --remote-port /dev/ttyS1 \
  --baudrate 921600 \
  --output-root build/board-e2e \
  --stage physical-uart-921600-final
```

Expected: four directions/cases pass exact CRC/SHA; summary reports latency distributions and no fallback/recovery.

- [ ] **Step 4: Record final evidence**

Append the new production/validation SHA-256 values, config default evidence, board-gate directory, physical-UART summary directory, frame counts and latency table to `docs/verification/incremental-board-e2e-results.md`. Do not rewrite historical results.

- [ ] **Step 5: Run final verification and commit**

```bash
git diff --check
git status --short
git add docs/verification/incremental-board-e2e-results.md
git commit -m "docs: record 921600 UART board validation"
git push origin validation/incremental-board-e2e
```

Expected: committed source/doc files only, ignored evidence remains under `build/`, push succeeds, and local/remote branch SHAs match.
