# Dual-board time synchronization and UART bridge design

Date: 2026-08-09  
Branch: `validation/incremental-board-e2e`

## 1. Scope and accepted requirements

This design covers the next incremental implementation and board-level
validation cycle for the nRF52840 wireless UART bridge. It addresses:

- 1 Hz PPS generation and wireless phase synchronization;
- master external PPS plus NMEA UTC disciplining;
- local-time fallback and holdover behavior;
- best-effort master-to-many-slave broadcast data;
- collision-free, per-slave polled uplink;
- independent internal slave queues while preserving a raw physical-UART
  output;
- persistent role, group, UART, and time-source parameters;
- ordinary-device group isolation; and
- incremental native, CDC, and board-level verification.

The product target is a stable locked slave PPS phase error of at most 20 µs
relative to the external PPS (or master PPS when no external reference is
present). This is a stable-state acceptance limit; acquisition, hard
re-alignment, and holdover are reported separately and are not counted as
locked-state measurements.

The design does not provide cryptographic message authentication, absolute UTC
after a cold boot with no external time source, or an externally distinguishable
slave ID on the raw master UART output.

## 2. Time source and PPS architecture

### 2.1 Hardware interfaces

The XIAO nRF52840 pin assignment is:

- `P0.02 / D0`: external PPS input, 3.3 V CMOS, active-high rising edge;
- `P0.03 / D1`: generated PPS output, active-high;
- `P0.05 / D5`: UART1 RX from the GNSS NMEA transmitter;
- `P0.04 / D4`: optional UART1 TX;
- `P1.11 / P1.12`: existing UART0 transparent bridge.

External PPS is captured by GPIOTE→GPPI→TIMER3 CC4. The capture occurs in
hardware in the same 1 MHz timebase used by RADIO ADDRESS capture (CC3), so
interrupt latency is excluded from the phase observation. The current
timebase channel abstraction must expose the nRF52840 TIMER3 CC4 channel
without moving the existing radio or PPS compare channels.

UART1 parses bounded NMEA lines. The default baudrate is 9600 and is a reboot-
effective parameter. Accepted sentences have a valid NMEA checksum; RMC
sentences must have active status, while ZDA sentences must contain valid date
and time fields. A sentence received 0–900 ms after a captured PPS is associated
with the immediately preceding PPS edge.

### 2.2 Master state machine

`time_source_mode` is a reboot-effective NVS parameter: `0=local` (default) or
`1=external`.

- `LOCAL`: output a continuous local 1 Hz PPS and broadcast local monotonic
  seconds. UTC is explicitly `UTC_INVALID`.
- `EXTERNAL_ACQUIRING`: output local PPS while waiting for valid PPS/NMEA
  pairs. The first valid pair supplies a direct phase correction; three
  consecutive valid pairs are required before reporting `LOCKED`.
- `EXTERNAL_LOCKED`: external PPS and UTC are both accepted. A missing PPS or
  missing NMEA input for three consecutive seconds leaves this state.
- `HOLDOVER`: continue the calibrated local oscillator phase and increment the
  last known UTC value. The state is exposed as `HOLDOVER`, so no locked-state
  phase or UTC accuracy is claimed. A cold boot that never obtained external
  time remains `UTC_INVALID`.

When an external edge is used for direct re-alignment, the next output edge is
skipped if it would be less than 500 ms after the preceding output edge. The
following external second is then used. This prevents a double pulse while
allowing a fast lock.

PPS output is fixed at a 1,000,000 µs period. The default active-high pulse
width is 100 ms, configurable through `pps_width_us` from 1–500 ms. The old
`pps_period_us` setting is migrated to a fixed 1 Hz value or exposed as read-
only; a persisted value cannot change the product frequency.

### 2.3 UTC propagation and phase budget

The synchronization discovery frame gains:

- the UTC second corresponding to the advertised next PPS, encoded as a signed
  64-bit Unix/POSIX UTC second; and
- a time-quality value (`UTC_INVALID`, `LOCKED`, or `HOLDOVER`).

The frame remains well below the 252-byte ESB payload limit. The protocol
version is incremented so old and new firmware do not silently interoperate.

The phase budget is allocated as an engineering target, not a claim before
measurement: master output relative to external PPS ≤5 µs, and the wireless
timestamp, calibrated radio delay, filter, and slave output contribute ≤15 µs.
`radio_delay_us` must be consumed by the offset calculation, and measured drift
must affect the master-to-local conversion. Missed-frame aging must run in the
runtime so a vanished master cannot leave a slave permanently marked `LOCKED`.

## 3. UART bridge and radio scheduling

### 3.1 Master-to-slave data

Business downlink data is sent once as an ESB `no_ack` broadcast to all active
slaves. Membership discovery and assignment control traffic retain their
existing reliability because they establish the node lease and address.

Downlink data keeps a stream epoch and sequence number only for duplicate,
gap, and loss accounting. The downlink history window, per-slave ACK bitmap,
repair action, and `SKIP_TO` action are removed. A failed local payload enqueue
may be retried; a packet already transmitted is never repaired over the air.

A slave delivers a valid new data frame immediately. A sequence gap increments
its loss counter but does not block later frames waiting for the missing one.
Old or duplicate frames are discarded without affecting the byte stream.

### 3.2 Slave-to-master data

The master polls active slaves in round-robin order. A slave responds only to
its own poll using an ACK payload. Uplink reliability is retained independently
of downlink reliability:

- one outstanding data block per slave;
- the slave repeats that block until the next poll acknowledges its sequence;
- the master suppresses duplicate delivery;
- the poll carries the last acknowledged uplink sequence and a byte credit.

This schedule prevents slave-to-slave transmission and avoids RF contention.

### 3.3 Independent queues and raw output

The current single `master_rx_queue` is replaced with one queue per node. Each
entry records one complete wireless payload block and its length. The output
scheduler takes at most one complete record per node per round-robin turn.

The UART TX path becomes a record queue: one asynchronous `uart_tx()` request
contains one slave record, and the next request starts immediately after the
previous completion. No artificial idle delimiter is inserted. Consequently,
the raw physical UART output may be indistinguishable across slave sources;
external source identification is deliberately outside the interface.

Queue overflow rejects a complete incoming record and increments a per-node
counter. It never truncates an existing record or drops the oldest bytes to
make room for newer data. CDC validation uses the same queues but is an
alternate input source to the physical UART, not a concurrent merged source.

## 4. Parameters, persistence, and grouping

Defaults are loaded into RAM before settings access. A valid persisted value
overrides its default; missing, malformed, or out-of-range entries leave the
default in place. Failure to initialize or load the settings backend logs the
condition and permits default-configured operation. A failed save reports an
error and does not claim that the value was persisted.

All communication, role, group, UART, and PPS parameters are reboot-effective.
Only status logging and LED parameters are runtime-effective, and their
callbacks must actually update the consumer. NVS operations use a blocking
mutex or serialized worker context, never a spinlock held across a potentially
blocking settings call.

The CDC parameter shell accepts and prints `group_key` as 32 hexadecimal
characters. `uart_baudrate` is passed to the physical UART configuration;
compile-time ring capacity is reported as fixed rather than pretending that
`uart_ring_size` resizes static storage.

The same firmware image selects master/slave behavior using `role_id=0` for
master and `1..3` for slave node IDs. `group_key + group_id` derive the RF
broadcast, node, temporary-assignment, and discovery-slot values. `group_id`
also selects one of 40 RF channels, so groups with equal channel indices can
still create physical RF interference but cannot normally join one another.
Every received sync frame is checked for `group_id` before any filter, session,
or discovery state is changed. The isolation goal is ordinary-device
non-cross-connection, not anti-spoofing.

## 5. Verification and recovery plan

Implementation is split into independently committed stages:

1. external PPS/NMEA capture, UTC state machine, fixed 1 Hz PPS, and native
   time-source tests;
2. UTC fields and quality in sync frames, group filtering, drift/radio-delay
   use, and sync aging;
3. best-effort broadcast downlink with no repair/skip and native loss tests;
4. per-slave uplink records, round-robin polling, record TX queue, and native
   multi-slave tests;
5. parameter fallback, bytes shell support, effective baud/PPS settings,
   NVS locking, and grouping tests;
6. dual-board CDC/UART bridge, cold-boot recovery, and documentation updates.

Before each stage is committed:

- native bridge and synchronization tests must pass;
- both connected boards are flashed from the current branch and identified by
  USB serial number;
- CDC logs are captured for role, group, time source, sync state, queue drops,
  and radio diagnostics;
- watchdog and retained radio diagnostics remain enabled for automatic
  recovery from a hang;
- a failed run records the retained reset reason and last radio action before
  any reset/retry.

Functional board acceptance covers local/external state transitions, two-board
bridge traffic, broadcast loss accounting, per-slave queue isolation, NVS
precedence, role/group changes, and repeated cold boots. Electrical acceptance
of the 1 Hz waveform, 100 ms pulse width, and ≤20 µs locked phase error requires
external PPS wiring and simultaneous oscilloscope measurement.

