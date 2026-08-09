# Kconfig and board-configuration audit

This audit covers every application-owned symbol that existed before the
configuration cleanup, every Zephyr/NCS symbol explicitly selected by the
production `prj.conf`, the validation fragments, unit-test fragments, and the
application board overlay. Runtime deployment choices remain in the
eight-entry NVS parameter table; Kconfig is retained only for compiled
defaults, memory sizing, protocol timing, hardware output, diagnostics, or
explicit validation variants.

## Application-owned symbols

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

The 15 retained application defaults are no longer repeated in `prj.conf`.
Their effective values still come from the defaults in the root `Kconfig`, so
there is one compiled-default source instead of two synchronized copies.

## Zephyr and NCS production symbols

| Symbol | Consumers or dependency | Decision | Reason |
| --- | --- | --- | --- |
| `GPIO` | PPS input/output and heartbeat GPIO APIs | Keep explicit | Required application driver family. |
| `SERIAL` | Data UART, time UART, CDC shell | Keep explicit | Required application driver family even though the board and shell also enable it. |
| `UART_ASYNC_API` | `uart_bridge.c`, `time_uart.c` | Keep | Both physical UART instances use the asynchronous API. |
| `ESB` | `radio_transport.c` | Keep | The only wireless transport implementation. |
| `ESB_MAX_PAYLOAD_LENGTH=252` | Link protocol and ESB payload buffers | Keep | Defines the validated maximum frame capacity. |
| `ESB_PIPE_COUNT=4` | Four configured ESB prefixes | Keep | Matches `esb_set_prefixes(..., 4)`. |
| `ESB_SYS_TIMER1` | ESB system timer | Keep | TIMER2 is used by PPS output and TIMER3 by the timebase, so the default TIMER2 choice would conflict. |
| `ESB_DYNAMIC_INTERRUPTS` | ESB RADIO/TIMER/EGU IRQ registration | Keep pending board study | Disabling it is a separate radio/cold-boot behavior change and is explicitly outside the conservative cleanup. |
| `MPSL_DYNAMIC_INTERRUPTS` | Full-MPSL IRQ registration | Keep pending board study | Coupled to the deferred ESB dynamic-interrupt decision. |
| `ESB_MPSL_TIMESLOT=n` | ESB Kconfig default | Remove explicit assignment | Timeslots default off and no other symbol enables them. |
| `CRYPTO` | AES address derivation | Keep | Enables the Zephyr cipher API used by `radio_address_crypto.c`. |
| `CRYPTO_NRF_ECB` | `DEVICE_DT_GET_ONE(nordic_nrf_ecb)` | Keep explicit | The source directly requires the nRF ECB backend even though it currently defaults on. |
| `ENTROPY_GENERATOR` | `sys_rand32_get()` session generation | Keep | Selects the hardware entropy-backed random generator. |
| `HWINFO` | `hwinfo_get_device_id()` | Keep | Supplies the stable device identifier used during discovery. |
| `CONSOLE` | XIAO CDC console | Keep explicit | Product diagnostics are exposed over the board CDC console. |
| `UART_CONSOLE` | XIAO CDC console | Keep explicit | Makes the console transport requirement visible rather than relying only on board defaults. |
| `LOG` | All runtime modules | Keep | Production diagnostics and recovery evidence depend on logging. |
| `PRINTK` | No direct application call; Zephyr default is `y` | Remove explicit assignment | Repeating the default does not select application behavior. |
| `LOG_MODE_DEFERRED` | Logging mode choice default | Remove explicit assignment | Deferred mode is already the active Zephyr default. |
| `LOG_BUFFER_SIZE=4096` | Deferred logger | Keep | Preserves burst diagnostics needed for cold-boot and radio recovery analysis. |
| `LOG_PROCESS_THREAD_STARTUP_DELAY_MS=100` | CDC logger startup | Keep | Overrides the XIAO CDC default of 4000 ms so early boot evidence is processed promptly. |
| `NRFX_TIMER` | Selected by `ESB_SYS_TIMER1` | Remove explicit assignment | The ESB timer choice selects it. |
| `NRFX_GPIOTE` | Direct nrfx GPIOTE use in PPS input | Keep explicit | PPS capture uses nrfx channel allocation and trigger APIs. |
| `NRFX_GPPI` | Selected by `ESB`; used by PPS and radio capture | Remove explicit assignment | ESB already selects the GPPI helper required by the application. |
| `CLOCK_CONTROL` | `timebase.c` HFCLK request | Keep explicit | The application directly uses the Nordic clock-control API. |
| `MPSL` | Selected by `ESB` | Remove explicit assignment | ESB owns the MPSL/FEM dependency choice. |
| `MAIN_STACK_SIZE=4096` | `main()` and nested initialization/log calls | Keep pending measurement | Reducing it requires on-board high-water evidence and is outside the conservative cleanup. |
| `SYSTEM_WORKQUEUE_STACK_SIZE=4096` | Zephyr/NCS/USB system work | Keep pending measurement | Reducing it requires on-board high-water evidence and is outside the conservative cleanup. |
| `THREAD_NAME` | Implied by the retained kernel shell | Remove explicit assignment | The sole application naming call was unused; the effective symbol remains enabled by `KERNEL_SHELL`. |
| `SETTINGS` | Parameter persistence and boot guard | Keep | Required settings core. |
| `SETTINGS_RUNTIME` | No `settings_runtime_*` caller | Remove | This builds an unused runtime accessor backend; dynamic handlers are a separate default symbol. |
| `FLASH` | NVS storage | Keep | Required by NVS and internal-flash writes. |
| `FLASH_MAP` | Settings NVS backend choice | Keep | `SETTINGS_NVS` depends on flash-map partition access. |
| `MPU_ALLOW_FLASH_WRITE` | Internal NVS writes | Keep | Allows runtime writes to the protected internal flash mapping. |
| `NVS` | Settings backend | Keep | Makes `SETTINGS_NVS` eligible instead of silently selecting `SETTINGS_NONE`. |
| `SHELL` | Parameter and time/status commands | Keep | Required runtime configuration interface. |
| `SHELL_BACKEND_SERIAL` | CDC shell transport | Keep explicit | Product configuration requires the serial CDC backend. |
| `SHELL_PROMPT_UART="uart:~$ "` | Shell default | Remove explicit assignment | It is byte-for-byte equal to the Zephyr default. |
| `SHELL_CMD_BUFF_SIZE=128` | Runtime commands | Keep | Fits the longest supported parameter command while reducing the 256-byte default. |
| `SHELL_PRINTF_BUFF_SIZE=128` | Shell formatted output | Keep | Preserves validated output chunking for status and automation parsing. |
| `WATCHDOG` | `watchdog_init()` and main-loop feed | Keep | Required automatic recovery from hangs. |
| `WDT_NRFX` | Defaults on for enabled nRF `wdt0` | Remove explicit assignment | The target driver is selected by `WATCHDOG` plus devicetree. |
| `UART_0_INTERRUPT_DRIVEN=n` | Data-UART async API | Keep | Overrides the shell-selected global interrupt mode for physical uart0. |
| `UART_0_ASYNC` | Defaults on after uart0 interrupt mode is disabled | Remove explicit assignment | The per-instance default produces the same effective configuration. |
| `UART_1_INTERRUPT_DRIVEN=n` | Time-UART async API | Keep | Overrides the shell-selected global interrupt mode for physical uart1. |
| `UART_1_ASYNC` | Defaults on after uart1 interrupt mode is disabled | Remove explicit assignment | The per-instance default produces the same effective configuration. |
| `UART_USE_RUNTIME_CONFIGURE` | `uart_configure()` for both physical UARTs | Keep | Nordic defconfig otherwise disables the API and returns `-ENOTSUP`. |
| `REBOOT` | cold reboot and 1200-baud UF2 entry | Keep | Required recovery and update behavior. |
| `UART_LINE_CTRL` | CDC baud-rate callback | Keep explicit | Required to recognize the 1200-baud UF2 reset request. |

## Validation, unit-test, and board configuration

`validation.conf` and `loss-validation.conf` each contain one purpose-specific
symbol and remain separate from production. The five unit-test fragments keep
only the subsystem requirements of their compiled test targets; the three
redundant `PRINTK=y` assignments were removed because `PRINTK` already defaults
on for those targets.

In `boards/xiao_ble_nrf52840.overlay`, TIMER1, TIMER3, both UART aliases, both
PPS aliases, UART1, and its pinctrl groups all have production consumers. The
unused `debug-out` alias and P0.28 GPIO node were removed. No timer, UART, PPS,
radio, watchdog, stack-size, or dynamic-interrupt behavior was changed by the
conservative cleanup.

## Conservative-cleanup verification

The production build generated after the cleanup was compared against the
preceding stage-3 production `.config`. The only effective-symbol change was
`SETTINGS_RUNTIME=y` to unset; every removed default or selected assignment
resolved to its previous value. Production and validation images built with
SHA-256 values `b243588e73dea55d9a7bc4c63726847e9f20d11502bc11dccbca209a02e9ba12`
and `c56973166cdebe11a3d9403b3a54432d9140d99f5b682ca8b99775b84c3a9dde`,
respectively.

All five native suites, 69 host tests, and the Python lint gate passed. The
two-board gate evidence is in
`build/board-e2e/20260809T155258.822355Z-cleanup-stage3-kconfig-conservative`:
both exact-ID flashes completed without retry, master discovered one slave,
the slave reached `LOCKED`, both 600-byte bridge directions passed before and
after one cold reboot per board, all queue/drop/input-error checks were zero,
and the five-second steady-state gate completed with zero recovery.
