# Dual-board time synchronization and UART bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement and board-verify the approved 1 Hz external/local UTC time source, best-effort broadcast downlink, per-slave polled uplink, persistent configuration, and ordinary-device group isolation.

**Architecture:** Keep the existing ESB discovery/session framework and hardware PPS output, but add pure NMEA parsing, a UTC/holdover state machine, and hardware PPS input on the same TIMER3 timebase. Replace only business downlink reliability with `no_ack` broadcast, retain unicast poll/ACK for slave uplink, and replace the single master uplink byte queue with per-node record queues feeding a record-aware UART TX path.

**Tech Stack:** nRF Connect SDK/Zephyr, nRF52840 TIMER3/GPIOTE/GPPI, Nordic ESB at 2 Mbps, C, ztest/native_sim, Zephyr settings/NVS, USB CDC shell, Python/pyserial UF2 tooling, and external oscilloscope measurement for the 20 µs phase gate.

**Workspace:** Work only in `/home/hv/projects/pps` on `validation/incremental-board-e2e`; do not create a worktree. Commit every completed stage before moving to the next stage.

## File map and boundaries

- `src/nmea_parser.[ch]`: pure, bounded NMEA RMC/ZDA checksum/date/time parser; no Zephyr device access.
- `src/utc_clock.[ch]`: local/external/acquiring/locked/holdover state machine, UTC second mapping, 3-second timeout, and publication quality.
- `src/pps_input.[ch]`: P0.02 GPIOTE→GPPI→TIMER3 CC4 capture and event handoff to a thread context.
- `src/time_uart.[ch]`: UART1 async RX on P0.05/P0.04, line buffering, and delivery of parsed NMEA sentences to `utc_clock`.
- `src/timebase.[ch]`: expose TIMER3 CC4 while preserving CC0 current-time capture, CC1/CC2 PPS compares, and CC3 radio capture.
- `src/record_queue.[ch]`: fixed-capacity byte storage plus complete-record lengths; reject whole records on overflow.
- `src/link_protocol.[ch]`: protocol v2 sync UTC fields and reduced uplink ACK wire layout.
- `src/link_scheduler_core.[ch]`: no-ACK business broadcast, direct slave gap handling, per-node master queues, and round-robin poll/output APIs.
- `src/uart_bridge.[ch]`: configurable physical UART baud and record-aware asynchronous TX descriptors.
- `src/bridge_runtime.[ch]`: time-source polling, UTC publication injection, group filtering, scheduler record draining, and expanded diagnostics.
- `src/param_config.[ch]`, `src/param_defs.h`, `src/param_shell.c`: NVS fallback/mutex fixes, new time parameters, bytes shell support, and accurate reboot/runtime flags.
- `src/main.c`, `src/status.[ch]`, `src/heartbeat_led.[ch]`: initialization order, fixed 1 Hz enforcement, runtime status/LED application, and state logging.
- `boards/xiao_ble_nrf52840.overlay`, `Kconfig`, `prj.conf`, `CMakeLists.txt`: UART1/PPS aliases, parameter ranges, feature source list, and test/production configuration.
- `tests/bridge_logic/src/`: parser, clock, protocol, scheduler, record queue, parameter, address, and loss-injection native tests.
- `tests/*_static_check.sh`: fail-closed checks for timer channels, UART1 pinout, no repair actions, group filtering, and production CDC separation.
- `tools/board_e2e.py`, `tests/test_board_e2e.py`: fixed-USB-ID log capture, command timeout/recovery, and repeatable dual-board CDC validation.
- `docs/verification/incremental-board-e2e-results.md`, `README.md`: stage results, wiring, parameter semantics, and measurement limitations.

The approved design covers several coupled areas, so the tasks below are
organized as independently testable sub-projects. Each sub-project ends in a
separate commit and can be stopped or reverted without losing earlier stages.

---

### Task 1: Establish the clean implementation baseline

**Files:** Read-only `git`, `docs/superpowers/specs/2026-08-09-dual-board-time-sync-uart-bridge-design.md`, `build.sh`, `CMakeLists.txt`, `tests/bridge_logic/`, `tests/sync_logic/`.

- [ ] **Step 1: Verify the approved design commit and clean workspace**

Run:

```bash
git status --short --branch
git log -2 --oneline --decorate
git show --stat --oneline effe24c
```

Expected: branch is `validation/incremental-board-e2e`, the design commit is an
ancestor of `HEAD`, and the worktree is clean before implementation starts.

- [ ] **Step 2: Run the current native suites and static checks**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/zephyr/zephyr.exe
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/sync_logic -d build/tests/sync_logic
./build/tests/sync_logic/zephyr/zephyr.exe
for check in tests/*_static_check.sh; do bash "$check"; done
```

Expected: bridge 93/93 and sync 15/15 (or the current test counts printed by
the suites), all active static checks pass, and no baseline behavior is changed.

- [ ] **Step 3: Commit the baseline record**

Append the exact commands, test counts, toolchain paths, and `git rev-parse
HEAD` to `docs/verification/incremental-board-e2e-results.md`, then commit:

```bash
git add docs/verification/incremental-board-e2e-results.md
git commit -m "test: record approved implementation baseline"
```

---

### Task 2: Add pure NMEA parsing and UTC clock behavior (TDD)

**Files:** Create `src/nmea_parser.[ch]`, `src/utc_clock.[ch]`; create and modify `tests/bridge_logic/src/test_nmea_parser.c`, `tests/bridge_logic/src/test_utc_clock.c`, `tests/bridge_logic/CMakeLists.txt`.

- [ ] **Step 1: Write failing NMEA parser tests**

Add tests with these exact cases:

```c
ZTEST(nmea_parser, test_rmc_active_is_decoded)
{
	struct rb_nmea_utc utc;
	const char *sentence =
		"$GPRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A*6A\r\n";
	zassert_ok(rb_nmea_parse_sentence(sentence, strlen(sentence), &utc));
	zassert_equal(utc.year, 1994);
	zassert_equal(utc.month, 3);
	zassert_equal(utc.day, 23);
	zassert_equal(utc.hour, 12);
	zassert_equal(utc.minute, 35);
	zassert_equal(utc.second, 19);
}

ZTEST(nmea_parser, test_bad_checksum_and_void_rmc_are_rejected)
{
	struct rb_nmea_utc utc;
	const char *bad = "$GPRMC,123519.00,A,4807.038,N,01131.000,E,0,0,230394,,,A*00\r\n";
	const char *void_fix = "$GPRMC,123519.00,V,,,,,,,230394,,,N*00\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad, strlen(bad), &utc), -EBADMSG);
	zassert_equal(rb_nmea_parse_sentence(void_fix, strlen(void_fix), &utc), -EAGAIN);
}

ZTEST(nmea_parser, test_zda_is_decoded)
{
	struct rb_nmea_utc utc;
	const char *sentence = "$GPZDA,201530.00,04,07,2002,00,00*60\r\n";
	zassert_ok(rb_nmea_parse_sentence(sentence, strlen(sentence), &utc));
	zassert_equal(utc.year, 2002);
	zassert_equal(utc.month, 7);
	zassert_equal(utc.day, 4);
	zassert_equal(utc.second, 30);
}
```

Run the bridge native build. Expected: compile failure because the parser API
does not exist.

- [ ] **Step 2: Implement the bounded parser API**

Define these public types and functions in `src/nmea_parser.h`:

```c
struct rb_nmea_utc {
	uint16_t year;
	uint8_t month, day, hour, minute, second;
	uint16_t millisecond;
};

int rb_nmea_parse_sentence(const char *line, size_t len,
				   struct rb_nmea_utc *utc);
int rb_nmea_utc_to_unix(const struct rb_nmea_utc *utc, int64_t *seconds);
```

Reject lines without `$`, `*HH`, CR/LF-safe bounded length, invalid hex,
checksum mismatch, impossible calendar fields, inactive RMC status, and
sentences other than RMC/ZDA. Parse fractional seconds but require the integer
second for PPS association. Use UTC calendar arithmetic rather than local-time
library calls so native and firmware results are identical.

- [ ] **Step 3: Write failing UTC clock state tests**

Use a fake 1 MHz tick source and callback recorder. Cover:

```c
ZTEST(utc_clock, test_local_mode_is_utc_invalid)
ZTEST(utc_clock, test_first_external_pair_hard_realigns)
ZTEST(utc_clock, test_three_pairs_enter_locked)
ZTEST(utc_clock, test_missing_pps_or_nmea_for_three_seconds_enters_holdover)
ZTEST(utc_clock, test_holdover_increments_last_utc)
ZTEST(utc_clock, test_recovery_skips_edge_under_500ms)
ZTEST(utc_clock, test_pair_after_900ms_is_rejected)
```

Assert that `LOCAL` never reports UTC valid, the first valid pair requests a
phase reset, three consecutive pairs are required for `LOCKED`, either input
missing for 3,000,000 ticks enters `HOLDOVER`, and the recovery target is at
least 500,000 ticks after the previous output edge.

- [ ] **Step 4: Implement `utc_clock` with an injectable interface**

Define the state and publication interface in `src/utc_clock.h`:

```c
enum rb_utc_state { RB_UTC_LOCAL, RB_UTC_ACQUIRING,
	RB_UTC_LOCKED, RB_UTC_HOLDOVER, RB_UTC_INVALID };
enum rb_time_quality { RB_TIME_UTC_INVALID, RB_TIME_LOCKED,
	RB_TIME_HOLDOVER };
struct rb_utc_clock_config { bool external_mode; uint32_t loss_timeout_us; };
struct rb_utc_publication { int64_t next_pps_utc_seconds;
	enum rb_time_quality quality; bool valid; };
void rb_utc_clock_init(struct rb_utc_clock *clock,
			       const struct rb_utc_clock_config *config);
int rb_utc_clock_note_pair(struct rb_utc_clock *clock,
			   uint64_t pps_tick, int64_t utc_second);
void rb_utc_clock_tick(struct rb_utc_clock *clock, uint64_t now_tick);
int rb_utc_clock_publication(const struct rb_utc_clock *clock,
			     uint64_t now_tick,
			     struct rb_utc_publication *publication);
enum rb_utc_state rb_utc_clock_state(const struct rb_utc_clock *clock);
```

Keep GPIO, UART, and Zephyr calls out of this module. The phase-reset callback
receives a future local tick and is responsible for the existing `pps_output`
500 µs arm-ahead constraint.

- [ ] **Step 5: Run the focused native tests and commit**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/zephyr/zephyr.exe
```

Expected: parser and UTC state tests pass; existing bridge tests remain green.
Commit:

```bash
git add src/nmea_parser.* src/utc_clock.* tests/bridge_logic/src/test_nmea_parser.c \
  tests/bridge_logic/src/test_utc_clock.c tests/bridge_logic/CMakeLists.txt
git commit -m "feat: add external NMEA UTC clock state machine"
```

---

### Task 3: Capture external PPS and NMEA on the board

**Files:** Modify `boards/xiao_ble_nrf52840.overlay`, `Kconfig`, `prj.conf`, `CMakeLists.txt`, `src/timebase.[ch]`; create `src/pps_input.[ch]`, `src/time_uart.[ch]`; create `tests/pps_input_static_check.sh`, `tests/time_uart_static_check.sh`.

- [ ] **Step 1: Add failing hardware/static checks**

The checks must assert the exact aliases and channel allocation:

```bash
rg -q 'pps-in = &pps_in' boards/xiao_ble_nrf52840.overlay
rg -q 'time-uart = &uart1' boards/xiao_ble_nrf52840.overlay
rg -q 'gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>' boards/xiao_ble_nrf52840.overlay
rg -q 'NRF_TIMER.*CC4|PPS_INPUT_CAPTURE_CHANNEL.*4' src/pps_input.c src/timebase.c
rg -q 'NRF_PSEL\(UART_RX, 0, 5\)' boards/xiao_ble_nrf52840.overlay
```

Run both scripts before implementation and confirm they fail on the missing
UART1 alias/channel symbols.

- [ ] **Step 2: Add UART1 devicetree and Kconfig configuration**

Add to the overlay:

```dts
/ {
	aliases { time-uart = &uart1; };
};

&uart1 {
	compatible = "nordic,nrf-uarte";
	status = "okay";
	pinctrl-0 = <&uart1_default>;
	pinctrl-1 = <&uart1_sleep>;
	pinctrl-names = "default", "sleep";
};

&pinctrl {
	uart1_default: uart1_default {
		group1 { psels = <NRF_PSEL(UART_TX, 0, 4)>; };
		group2 { psels = <NRF_PSEL(UART_RX, 0, 5)>; bias-pull-up; };
	};
	uart1_sleep: uart1_sleep {
		group1 { psels = <NRF_PSEL(UART_TX, 0, 4)>,
			<NRF_PSEL(UART_RX, 0, 5)>; low-power-enable; };
	};
};
```

Enable UART1 async mode without changing UART0 async mode. Add the reboot-
effective `time_uart_baudrate` parameter with default 9600 and range 1200–115200.

- [ ] **Step 3: Expose TIMER3 CC4 and implement PPS input capture**

Change `TIMEBASE_CHANNEL_COUNT` to 6 while retaining `TIMEBASE_CAPTURE_CHANNEL`
0, compare channels 1/2, and radio capture channel 3. Define
`TIMEBASE_PPS_INPUT_CAPTURE_CHANNEL 4u`. In `pps_input.c`, configure a GPIOTE
low-to-high event for `DT_ALIAS(pps_in)`, allocate one GPPI connection from the
GPIOTE event to `timebase_timer_capture_task_address(4)`, and put the captured
64-bit tick into a bounded `k_msgq` from a lightweight ISR/event callback. The
worker-facing API is:

```c
int pps_input_init(void);
int pps_input_poll(uint64_t *capture_tick, k_timeout_t timeout);
uint64_t pps_input_count(void);
```

Do not parse NMEA or call settings/PPS reset from the hardware event path.

- [ ] **Step 4: Implement asynchronous NMEA line ingestion**

`time_uart_init(uint32_t baudrate)` configures `DT_ALIAS(time_uart)` for 8-N-1,
double DMA buffers, and a bounded line accumulator. On `UART_RX_RDY`, append
bytes until CR/LF; reject overlong lines and increment a parse-drop counter. The
thread-facing API is:

```c
int time_uart_init(uint32_t baudrate);
size_t time_uart_read_line(char *line, size_t max_len);
const struct rb_time_uart_stats *time_uart_stats_get(void);
```

The UART callback only copies bytes and wakes the bridge thread. It must never
call `rb_nmea_parse_sentence` or NVS APIs.

- [ ] **Step 5: Run static checks and build both board images**

Run:

```bash
bash tests/pps_input_static_check.sh
bash tests/time_uart_static_check.sh
./build.sh master -d build/time-source-master
./build.sh slave -d build/time-source-slave
```

Expected: checks pass, both UF2 images are generated, and the build DTS shows
UART1 RX on P0.05 and PPS input on P0.02.

- [ ] **Step 6: Commit the hardware time-source stage**

```bash
git add boards/xiao_ble_nrf52840.overlay Kconfig prj.conf CMakeLists.txt \
  src/timebase.* src/pps_input.* src/time_uart.* \
  tests/pps_input_static_check.sh tests/time_uart_static_check.sh
git commit -m "feat: add external PPS capture and NMEA UART input"
```

---

### Task 4: Propagate UTC and integrate time-source states

**Files:** Modify `src/link_protocol.[ch]`, `src/link_scheduler_core.[ch]`, `src/wireless_time_sync.[ch]`, `src/bridge_runtime.[ch]`, `src/main.c`, `src/status.[ch]`, `CMakeLists.txt`; modify `tests/bridge_logic/src/test_link_protocol.c`, `test_master_scheduler.c`, `test_slave_scheduler.c`, `test_wireless_time_sync.c`, `tests/sync_logic/src/main.c`.

- [ ] **Step 1: Write failing protocol tests**

Add a round-trip test that sets:

```c
frame.next_pps_utc_seconds = 1700000000;
frame.time_quality = RB_TIME_LOCKED;
```

and asserts that encode/decode preserves both fields, rejects protocol version
1, and rejects a wire length other than the new fixed sync length.

- [ ] **Step 2: Add protocol-v2 UTC fields**

Set `RB_PROTOCOL_VERSION` to 2. Extend `struct rb_sync_discovery` with:

```c
int64_t next_pps_utc_seconds;
uint8_t time_quality;
```

Set `RB_SYNC_DISCOVERY_WIRE_SIZE` to 65 and add little-endian offsets plus a
static assertion. Validate `time_quality` against the three defined values.
Use `sys_put_le64`/`sys_get_le64` on the signed value without changing the wire
byte representation.

- [ ] **Step 3: Add scheduler time-publication injection**

Add this API to `link_scheduler_core.h`:

```c
struct rb_scheduler_time_publication {
	int64_t next_pps_utc_seconds;
	uint8_t time_quality;
};

void rb_scheduler_set_time_publication(
	struct rb_scheduler_core *core,
	const struct rb_scheduler_time_publication *publication);
```

`build_sync()` copies the publication into every master sync frame. Initialize
the default to `UTC_INVALID` and zero seconds so local mode remains explicit.

- [ ] **Step 4: Integrate the time source into runtime**

After parameters, `timebase_init`, and `pps_output_init`, initialize
`rb_utc_clock` and `pps_input`; initialize `time_uart` only in external mode.
In the bridge thread, poll captured PPS ticks and complete NMEA/PPS pairs in
thread context. On a phase-reset request, call `pps_output_reset_epoch()` only
with a future target at least 100 µs ahead. Update scheduler publication before
the next sync action.

In `runtime_apply_sync`, reject `frame.group_id != runtime_group_id` before
session reset, discovery-slot calculation, counter updates, or
`wireless_time_sync_slave_receive`. Apply the frame's UTC publication only after
all protocol/group/session checks pass.

- [ ] **Step 5: Fix filter drift and age handling**

Call `sync_filter_note_missed()`/`sync_filter_age()` from the bridge thread on
every scheduler tick. Change `sync_filter_master_to_local()` to apply the
measured drift term to the predicted next PPS. Add explicit stats for sync age,
UTC quality, NMEA parse drops, external PPS count, and holdover transitions.

- [ ] **Step 6: Update status and run native tests**

Add `time_source`, `utc_quality`, `utc_seconds`, `external_pps_count`,
`nmea_valid_count`, `nmea_drop_count`, and `holdover_count` to status output.
Run both native suites and all active static checks. Expected: existing sync
offset tests still pass, protocol round-trips pass, and local mode reports
`UTC_INVALID`.

- [ ] **Step 7: Commit the time synchronization stage**

```bash
git add src/link_protocol.* src/link_scheduler_core.* src/wireless_time_sync.* \
  src/bridge_runtime.* src/main.c src/status.* tests/bridge_logic \
  tests/sync_logic CMakeLists.txt
git commit -m "feat: add UTC publication and external time-source states"
```

---

### Task 5: Remove business downlink reliability while preserving control and uplink poll

**Files:** Modify `src/link_protocol.[ch]`, `src/link_scheduler_core.[ch]`, `src/link_window.[ch]` only where required for compile/test compatibility, `src/bridge_runtime.c`, `src/radio_transport.c`, `src/status.[ch]`, `tests/bridge_logic/src/test_link_protocol.c`, `test_master_scheduler.c`, `test_slave_scheduler.c`, `tests/radio_transport_static_check.sh`.

- [ ] **Step 1: Replace repair-oriented tests with fail-closed broadcast tests**

Remove tests whose expected action is `RB_ACTION_SEND_REPAIR` or
`RB_ACTION_SEND_SKIP_TO`. Add these cases:

```c
ZTEST(master_scheduler, test_broadcast_has_no_ack_and_no_history)
ZTEST(master_scheduler, test_broadcast_loss_does_not_schedule_repair)
ZTEST(slave_scheduler, test_downlink_gap_delivers_later_frame)
ZTEST(slave_scheduler, test_downlink_duplicate_is_not_delivered_twice)
```

The loss test must call the scheduler event path with a missing sequence and
assert that the next action is not repair or skip.

- [ ] **Step 2: Reduce protocol ACK state**

Change `struct rb_ack_uplink` to retain only `common`, `uplink_epoch`,
`uplink_sequence`, `drop_count`, and payload. Set
`RB_ACK_UPLINK_HEADER_SIZE` to 22, update offsets/static assertions, and remove
downlink ACK bitmap encode/decode fields. Keep the `rb_poll` uplink ACK base,
bitmap, credit, and poll sequence because they acknowledge slave uplink.

- [ ] **Step 3: Remove downlink history and repair actions**

Delete the scheduler's `downlink_history`, `downlink_slots`, peer
`repair_pending`/`skip_pending`/`missing_sequence` fields, and
`rb_scheduler_note_downlink_ack()`/`rb_scheduler_take_evicted()` APIs. Remove
`RB_ACTION_SEND_REPAIR`, `RB_ACTION_SEND_SKIP_TO`, and their runtime switch
cases. Leave `link_window.c` available for independent legacy/native tests only
until its last consumer is removed.

- [ ] **Step 4: Make slave data delivery gap-tolerant**

Replace `rb_rx_window_insert/pop` in `slave_handle_rx()` with a per-epoch
sequence gate: reject sequence numbers at or behind `last_downlink_sequence`,
increment `downlink_duplicate_count`; for a newer sequence, increment the gap
counter by the forward distance minus one, append the complete payload to the
slave UART queue, and record the new sequence. Reset the gate on ASSIGN/session
change.

- [ ] **Step 5: Verify radio action semantics**

Keep `action->no_ack = true` in `build_broadcast()` and add a static check that
business downlink actions call `radio_transport_send(..., true, ...)`. Keep
selective auto-ACK enabled globally so ASSIGN and POLL control exchanges work;
the payload-level `noack` bit, not a global ESB mode change, separates control
from business data.

- [ ] **Step 6: Run native/static verification and commit**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/zephyr/zephyr.exe
bash tests/radio_transport_static_check.sh
```

Expected: no repair/skip action is generated under injected loss, control
membership and uplink poll tests pass, and the native suite has no failures.
Commit:

```bash
git add src/link_protocol.* src/link_scheduler_core.* src/bridge_runtime.c \
  src/radio_transport.c src/status.* tests/bridge_logic tests/radio_transport_static_check.sh
git commit -m "feat: make business downlink best-effort broadcast"
```

---

### Task 6: Add per-slave record queues and record-aware UART TX

**Files:** Create `src/record_queue.[ch]`, modify `src/link_scheduler_core.[ch]`, `src/bridge_runtime.c`, `src/uart_bridge.[ch]`, `src/bridge_validation.[ch]`, `src/bridge_validation_shell.c`, `CMakeLists.txt`; create `tests/bridge_logic/src/test_record_queue.c`, modify `test_master_scheduler.c`, `test_slave_scheduler.c`, `tests/uart_bridge_static_check.sh`.

- [ ] **Step 1: Write record-queue unit tests**

Test that:

```c
ZTEST(record_queue, test_push_peek_pop_preserves_record_lengths)
ZTEST(record_queue, test_full_queue_rejects_whole_new_record)
ZTEST(record_queue, test_round_robin_reads_one_node_record_at_a_time)
```

Push node 1 bytes `{0x10,0x11}`, node 2 bytes `{0x20}`, then node 1 `{0x12}`;
peek/pop must return exactly those complete records in scheduler order and
never splice bytes from two nodes. A full data or descriptor ring returns
`-ENOSPC` and leaves all existing records unchanged.

- [ ] **Step 2: Implement the bounded `record_queue`**

Expose:

```c
struct rb_record_queue;
void rb_record_queue_init(struct rb_record_queue *queue,
			  uint8_t *storage, size_t storage_size,
			  uint16_t *lengths, size_t length_capacity);
int rb_record_queue_push(struct rb_record_queue *queue,
			 const uint8_t *data, size_t len);
int rb_record_queue_peek(const struct rb_record_queue *queue,
			    uint8_t *data, size_t max_len, size_t *record_len);
int rb_record_queue_pop(struct rb_record_queue *queue);
```

The implementation must reserve both payload bytes and one length slot before
copying, so overflow cannot create a partial record. Use fixed storage sized
from `RB_SCHEDULER_UART_QUEUE_SIZE` and `RB_ACK_UPLINK_PAYLOAD_MAX`.

- [ ] **Step 3: Replace the master merged queue**

Add one record queue per valid node ID in `rb_scheduler_core`. On receipt of an
ACK payload, append the payload as one record to that node's queue. Keep
duplicate detection and uplink ACK bookkeeping per peer. Add:

```c
int rb_scheduler_master_peek_record(struct rb_scheduler_core *core,
				    uint8_t *node_id, uint8_t *data,
				    size_t max_len, size_t *record_len);
int rb_scheduler_master_pop_record(struct rb_scheduler_core *core,
				   uint8_t node_id);
```

`peek` scans from `poll_cursor` in round-robin order without removing data;
`pop` removes exactly the record returned by the preceding peek. Expose
per-node record drop counters in bridge stats.

- [ ] **Step 4: Make UART TX record-aware**

Replace the single byte-only TX pending state in `uart_bridge.c` with a fixed
descriptor ring containing `{const/copy buffer, length, offset, busy}`. Add:

```c
int uart_bridge_write_record(const uint8_t *data, size_t len);
size_t uart_bridge_record_available(void);
```

`uart_bridge_write_record()` accepts or rejects the complete record. The async
callback starts only the current descriptor and starts the next descriptor on
`UART_TX_DONE`; `UART_TX_ABORTED` retains the current descriptor offset for
retry. Existing `uart_bridge_write()` remains a byte-stream wrapper for the
physical input path but is not used for slave-record output.

- [ ] **Step 5: Integrate non-destructive scheduler draining**

Change `runtime_push_uart_tx()` to peek a node record, call
`uart_bridge_write_record()`, and pop only after acceptance. If TX descriptor
space is unavailable, leave the scheduler record queued and return on the next
wake. CDC output capture records the accepted bytes but does not add a source
header to the user-visible stream.

- [ ] **Step 6: Run native and static verification, then commit**

Run the bridge native suite, `bash tests/uart_bridge_static_check.sh`, and the
validation CDC configuration check. Expected: all existing exact 600-byte CDC
tests remain green, record boundaries are preserved internally, and production
builds contain no validation shell symbols.

```bash
git add src/record_queue.* src/link_scheduler_core.* src/bridge_runtime.c \
  src/uart_bridge.* src/bridge_validation.* src/bridge_validation_shell.c \
  CMakeLists.txt tests/bridge_logic tests/uart_bridge_static_check.sh
git commit -m "feat: isolate slave uplink records and UART TX descriptors"
```

---

### Task 7: Make NVS parameters effective and enforce group isolation

**Files:** Modify `src/param_config.[ch]`, `src/param_defs.h`, `src/param_shell.c`, `src/main.c`, `src/bridge_runtime.c`, `src/uart_bridge.[ch]`, `src/heartbeat_led.[ch]`, `src/status.[ch]`, `Kconfig`, `prj.conf`; modify `tests/bridge_logic/src/test_param_config.c`, `test_config.c`, `test_master_scheduler.c`, `tests/bridge_runtime_static_check.sh`; create `tests/param_fallback_static_check.sh`.

- [ ] **Step 1: Add failing parameter tests**

Extend the mock settings backend to support byte values and injected init/load
errors. Add tests for:

```c
ZTEST(param_config, test_missing_backend_keeps_defaults)
ZTEST(param_config, test_group_key_set_get_round_trip)
ZTEST(param_config, test_time_source_mode_requires_reboot)
ZTEST(param_config, test_communication_parameters_require_reboot)
ZTEST(param_config, test_only_status_and_led_are_runtime)
```

Update old expectations so aggregation/sync/slot/lease parameters are reboot
effective and PPS width defaults to 100000 µs.

- [ ] **Step 2: Add parameter descriptors and safe persistence**

Add `RB_PARAM_TIME_SOURCE_MODE`, `RB_PARAM_TIME_UART_BAUDRATE`, and
`RB_PARAM_PPS_INPUT_DELAY_US` with NVS IDs `0x1011`, `0x1012`, and `0x1013`.
Use ranges `0..1`, `1200..115200`, and `0..1000` respectively. Set the default
time source to local, default time UART to 9600, and all communication/time
parameters to `RB_PARAM_FLAG_REBOOT_REQUIRED`.

Introduce `rb_param_persistence_available()` and make
`rb_param_config_init()` mark the cache initialized even when
`settings_subsys_init()` or `settings_load_subtree()` fails. Keep defaults and
log the failure. Replace `k_spinlock` around `settings_save_one()` and
`settings_delete()` with a `k_mutex` initialized during subsystem startup.

- [ ] **Step 3: Add bytes support to the CDC shell**

For `RB_PARAM_BYTES`, parse exactly 32 hexadecimal characters with
`rb_group_key_parse()`, call `rb_param_set_bytes()`, and print the value in
lowercase hex from `param get`. Reject whitespace, odd length, and non-hex input.

- [ ] **Step 4: Apply parameters at the real consumers**

Change `uart_bridge_init()` to accept the persisted baudrate and configure the
actual UART with it. Remove the runtime read of a mutable PPS period and pass
the fixed constant `1000000u` to `pps_output_start_periodic()`. Read time source,
NMEA baud, PPS width, radio delay, group ID/key, and role before starting radio.
Update the main status loop to re-read only status interval/LED values or use
explicit heartbeat setter APIs; never advertise runtime update for a parameter
whose consumer is unchanged.

- [ ] **Step 5: Add early group filtering**

In `runtime_apply_sync()`, put:

```c
if (frame.group_id != runtime_group_id) {
	bridge_stats.invalid_group_packets++;
	return;
}
```

before session reset, discovery slot calculation, counters, or filter updates.
Add a native test that delivers a validly encoded sync frame with a different
group and asserts that state, sequence, and relock count do not change.

- [ ] **Step 6: Run focused tests and commit**

Run:

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/zephyr/zephyr.exe
for check in tests/*_static_check.sh; do bash "$check"; done
```

Expected: parameter defaults/fallback, bytes shell parsing, group filtering,
and all existing bridge tests pass. Commit:

```bash
git add Kconfig prj.conf src/param_* src/main.c src/bridge_runtime.c \
  src/uart_bridge.* src/heartbeat_led.* src/status.* tests/bridge_logic \
  tests/param_fallback_static_check.sh tests/bridge_runtime_static_check.sh
git commit -m "fix: make persisted parameters and group filtering effective"
```

---

### Task 8: Add automated dual-board recovery and CDC regression

**Files:** Create `tools/board_e2e.py`, `tests/test_board_e2e.py`, `tests/board_e2e_static_check.sh`; modify `tools/flash_uf2.py` only if a tested helper is missing; modify `src/bridge_validation_shell.c`, `src/status.c`, `docs/verification/incremental-board-e2e-results.md`, `README.md`.

- [ ] **Step 1: Test fixed-ID serial discovery helpers**

Add Python unit tests with temporary `/dev/serial/by-id` and UF2 disk trees:

```python
def test_board_e2e_refuses_ambiguous_serial_id(): ...
def test_board_e2e_reconnects_after_1200_baud_reset(): ...
def test_board_e2e_records_timeout_before_recovery(): ...
```

Use the existing `tools.flash_uf2.serial_port_for()` and never select a board
by `/dev/ttyACM0` or `/dev/ttyACM1`.

- [ ] **Step 2: Implement bounded log/command transactions**

`tools/board_e2e.py` must accept explicit master/slave IDs, serial baud, command
timeout, cycle count, and validation UF2 paths. It opens each fixed-ID CDC
port, sends one shell command at a time, waits for the command echo/result, and
writes timestamped raw logs. On a command timeout it records the last received
bytes, invokes `flash_uf2.py --device-id <id> --retry 1` for that board only,
waits for application re-enumeration, and retries the command once. A second
timeout exits non-zero with both board logs preserved.

- [ ] **Step 3: Add validation commands for independent diagnostics**

Extend `bridge_test stats` with per-node uplink record counts/drops and add a
time-source status command showing mode, UTC quality, NMEA/PPS counts, age, and
holdover transitions. Keep these fields diagnostic-only; the bridge payload
remains raw.

- [ ] **Step 4: Run the dual-board CDC matrix**

Build validation firmware:

```bash
./build.sh master -d build/validation-master -- \
  -DOVERLAY_CONFIG=validation.conf
./build.sh slave -d build/validation-slave -- \
  -DOVERLAY_CONFIG=validation.conf
```

Run:

```bash
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/validation-master/zephyr/zephyr.uf2 \
  --slave-uf2 build/validation-slave/zephyr/zephyr.uf2 \
  --cycles 5 --bridge-length 600
```

Require master→slave and slave→master exact CDC verification, zero validation
queue drops, per-node output counters, and automatic recovery without a manual
double-click. Repeat with one board cold-reset while the other remains powered.

- [ ] **Step 5: Add loss and group-isolation runs**

Build a test-only image with `CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION=y`, drop
every third downlink DATA frame, and assert that loss counters rise while
repair/skip counters remain absent. Configure a second native scheduler with a
different group ID and inject its sync frame into the first; assert no sync
state, UTC publication, or lease state changes.

- [ ] **Step 6: Record final board evidence and commit**

Append exact firmware SHAs, board IDs, commands, log excerpts, reset counts,
loss-injection results, and any missing external PPS/scope fixture to
`docs/verification/incremental-board-e2e-results.md`. Update README wiring and
parameter tables. Run all native suites, Python tests, static checks, and both
pristine production/validation builds. Commit:

```bash
git add tools/board_e2e.py tests/test_board_e2e.py tests/board_e2e_static_check.sh \
  src/bridge_validation_shell.c src/status.c docs/verification/incremental-board-e2e-results.md README.md
git commit -m "test: automate dual-board bridge recovery and regression"
```

---

## Final plan self-review checklist

- **Spec coverage:** PPS input, NMEA association, local/external/HOLDOVER states,
  1 Hz/100 ms output, 20 µs measurement gate, UTC wire publication, best-effort
  broadcast, retained control/uplink polling, per-slave records, NVS fallback,
  effective baud, group filtering, recovery, and board evidence each have a
  task above.
- **Placeholder scan:** no unfinished-marker or unspecified implementation step
  is required; every task names files, APIs, tests, commands, and commit
  boundaries.
- **Type consistency:** `rb_utc_publication`, `rb_scheduler_time_publication`,
  `rb_record_queue`, `rb_scheduler_master_peek_record`, and
  `uart_bridge_write_record` are defined before their integration tasks use
  them.
- **Scope:** the plan retains the existing membership/session and radio profile
  machinery, removes only business downlink repair, and defers cryptographic
  authentication and raw-UART source framing as explicitly agreed.
