# Application Kconfig audit

This audit covers every application-owned symbol that existed before the
configuration cleanup. Zephyr/NCS symbols selected in `prj.conf` are outside
this table. Runtime deployment choices remain in the eight-entry NVS parameter
table; Kconfig is retained only for compiled defaults, memory sizing, protocol
timing, hardware output, diagnostics, or explicit validation variants.

| Symbol | Consumers | Decision | Reason |
| --- | --- | --- | --- |
| `RADIO_BRIDGE_VALIDATION_STAGE` | None | Remove | The incremental stage selector no longer has a build or source consumer. |
| `RADIO_BRIDGE_NEW_STACK` | Former `prj.conf` selection and validation dependencies only | Remove | ESB star radio is the only implementation, so the legacy stack selector cannot select behavior. |
| `RADIO_BRIDGE_GROUP_ID` | `app_config.h`, `param_config.c` | Keep | Supplies the compiled fallback for the persisted deployment group. |
| `RADIO_BRIDGE_GROUP_KEY` | root CMake validation, `app_config.h`, `param_config.c` | Keep | Supplies and validates the compiled fallback AES-128 group key. |
| `RADIO_BRIDGE_UART_BAUDRATE` | `app_config.h`, `param_config.c` | Keep | Supplies the compiled fallback for the persisted data-UART baud rate. |
| `TIME_UART_BAUDRATE` | `param_config.c` | Keep | Supplies the compiled fallback for the persisted external-time UART baud rate. |
| `RADIO_BRIDGE_UART_RING_SIZE` | `bridge_runtime.c`, `uart_bridge.c`, `app_config.h` | Keep | Changes static RAM capacity and queue backpressure behavior. |
| `RADIO_BRIDGE_AGGREGATION_TIMEOUT_US` | `bridge_runtime.c`, `uart_bridge.c` | Keep | Controls partial-record aggregation and scheduler timing at build time. |
| `RADIO_BRIDGE_SYNC_INTERVAL_US` | `bridge_runtime.c`, `app_config.h` | Keep | Controls discovery cadence, wireless time-sync cadence, and filter timing. |
| `RADIO_BRIDGE_MAX_SLAVES` | Former fixed-value assertion/fallback only | Remove | The supported topology is fixed at three slaves; a `3..3` option exposes no choice. |
| `RADIO_BRIDGE_RESPONSE_SLOT_COUNT` | `bridge_runtime.c`, `app_config.h` | Keep | Controls discovery collision capacity and scheduler timing. |
| `RADIO_BRIDGE_RESPONSE_SLOT_US` | `bridge_runtime.c` | Keep | Controls discovery response-slot timing. |
| `RADIO_BRIDGE_ASSIGNMENT_WINDOW_US` | `bridge_runtime.c` | Keep | Controls the assignment receive window in the radio protocol. |
| `RADIO_BRIDGE_LEASE_TIMEOUT_US` | `bridge_runtime.c` | Keep | Controls slave lease expiry in the radio protocol. |
| `RADIO_BRIDGE_IDLE_POLL_MAX_US` | `bridge_runtime.c` | Keep | Bounds idle polling latency in the scheduler. |
| `TIME_SYNC_PPS_PERIOD_US` | Former fixed-value descriptor/macro only | Remove | Standard PPS is unconditionally 1 Hz; a `1000000..1000000` option exposes no choice. |
| `TIME_SYNC_PPS_WIDTH_US` | `main.c`, `app_config.h` | Keep | Directly changes the physical PPS high-pulse width. |
| `TIME_SYNC_RADIO_DELAY_US` | `param_config.c` | Keep | Supplies the compiled fallback for persisted slave radio-delay calibration. |
| `TIME_SYNC_STATUS_INTERVAL_MS` | `main.c`, `app_config.h` | Keep | Controls diagnostic status cadence and watchdog feed-loop cadence. |
| `TIME_SYNC_LED_HEARTBEAT` | `main.c` | Keep | Enables or disables the board heartbeat diagnostic at build time. |
| `TIME_SYNC_LED_HEARTBEAT_PERIOD_MS` | `app_config.h`, `main.c` | Keep | Controls the compiled heartbeat diagnostic period. |
| `RADIO_BRIDGE_TEST_LOSS_INJECTION` | `radio_transport.c/.h`, `bridge_validation_shell.c`, `loss-validation.conf` | Keep | Explicit validation variant for radio loss and recovery testing; disabled in production. |
| `RADIO_BRIDGE_VALIDATION_CDC` | root CMake, `bridge_runtime.c/.h`, `validation.conf` | Keep | Explicit validation-only CDC data/time injection path; disabled in production. |

The retained timing/diagnostic symbols keep the former NVS safety bounds in
Kconfig. In particular, the status/heartbeat periods remain `100..10000 ms`,
radio-delay calibration remains `0..1000 us`, and scheduler interval/window/
lease values retain their prior validated ranges. The UART ring range starts at
16 KiB to match the application's fail-closed RAM-capacity requirement.
