import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tests.host.support import load_board_e2e, require_symbol

class ResolveSerialTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.serial_by_id = Path(self.temp_dir.name)
        self.addCleanup(self.temp_dir.cleanup)

    def test_complete_exact_id_reuses_flash_discovery(self):
        board_e2e = load_board_e2e()
        device_id = "DBE5C3D84EA2EC6F"
        link = self.serial_by_id / (
            f"usb-Zephyr_Project_CDC_ACM_serial_backend_{device_id}-if00"
        )
        link.symlink_to(Path("/dev/ttyACM7"))
        (
            self.serial_by_id
            / (f"usb-Zephyr_Project_CDC_ACM_serial_backend_{device_id}0-if00")
        ).symlink_to(Path("/dev/ttyACM8"))

        self.assertEqual(
            board_e2e.resolve_serial(device_id.lower(), self.serial_by_id),
            link,
        )

    def test_partial_id_fails_closed(self):
        board_e2e = load_board_e2e()
        with self.assertRaises(board_e2e.GateError):
            board_e2e.resolve_serial("DBE5C3D8", self.serial_by_id)

    def test_missing_id_fails_closed(self):
        board_e2e = load_board_e2e()
        with self.assertRaises(board_e2e.GateError):
            board_e2e.resolve_serial("DBE5C3D84EA2EC6F", self.serial_by_id)

    def test_ambiguous_id_fails_closed(self):
        board_e2e = load_board_e2e()
        device_id = "DBE5C3D84EA2EC6F"
        for interface in ("if00", "if01"):
            link = self.serial_by_id / (
                f"usb-Zephyr_Project_CDC_ACM_serial_backend_{device_id}-{interface}"
            )
            link.symlink_to(Path(f"/dev/ttyACM{interface[-1]}"))

        with self.assertRaises(board_e2e.GateError):
            board_e2e.resolve_serial(device_id, self.serial_by_id)


class CommandRecoveryTests(unittest.TestCase):
    def test_timeout_logs_tail_recovers_once_and_retries(self):
        board_e2e = load_board_e2e()
        attempts = []
        recoveries = []
        messages = []

        def run_once(command):
            attempts.append(command)
            if len(attempts) == 1:
                raise board_e2e.CommandTimeout(b"last received bytes")
            return "retry output"

        result = board_e2e.run_command_with_recovery(
            run_once,
            lambda: recoveries.append("recovered"),
            messages.append,
            "kernel uptime",
        )

        self.assertEqual(result, "retry output")
        self.assertEqual(attempts, ["kernel uptime", "kernel uptime"])
        self.assertEqual(recoveries, ["recovered"])
        self.assertIn("kernel uptime", messages[0])
        self.assertIn("last received bytes", messages[0])

    def test_second_timeout_raises_without_another_recovery(self):
        board_e2e = load_board_e2e()
        attempts = []
        recoveries = []
        messages = []

        def always_times_out(command):
            attempts.append(command)
            raise board_e2e.CommandTimeout("still waiting")

        with self.assertRaises(board_e2e.CommandTimeout):
            board_e2e.run_command_with_recovery(
                always_times_out,
                lambda: recoveries.append("recovered"),
                messages.append,
                "bridge_test stats",
            )

        self.assertEqual(attempts, ["bridge_test stats", "bridge_test stats"])
        self.assertEqual(recoveries, ["recovered"])
        self.assertEqual(len(messages), 2)
        self.assertIn("still waiting", messages[-1])


class HistoryCursorTests(unittest.TestCase):
    def test_history_since_keeps_fresh_marker_after_rollover(self):
        board_e2e = load_board_e2e()
        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(board_e2e, "HISTORY_MAX_BYTES", 24),
        ):
            raw_log = board_e2e.TimestampedRawLog(Path(temp_dir) / "board.raw.log")
            self.addCleanup(raw_log.close)
            board = board_e2e.BoardSession(
                "master", "DBE5C3D84EA2EC6F", Path("firmware.uf2"), 1.0, raw_log
            )
            board._remember(b"s" * 16)
            fresh_mark = board.history_mark()
            board._remember(b"guard-marker")
            board._remember(b"post")

            fresh = board.history_since(fresh_mark)

        self.assertIn(b"guard-marker", fresh)
        self.assertIn(b"post", fresh)


class ShellOutputTests(unittest.TestCase):
    def test_role_parser_accepts_current_param_output(self):
        board_e2e = load_board_e2e()
        output = "role_id = 1 (persisted, reboot)\r\n"
        self.assertEqual(require_symbol(board_e2e, "_parse_role")(output), 1)

    def test_role_parser_rejects_unrecognized_output(self):
        board_e2e = load_board_e2e()
        with self.assertRaises(board_e2e.GateError):
            require_symbol(board_e2e, "_parse_role")("role is slave\r\n")

    def test_drop_parser_requires_both_zero_fields(self):
        board_e2e = load_board_e2e()
        require_symbol(board_e2e, "_require_zero_drops")(
            "bridge_test stats input=0 output=0 injected=600 captured=600 "
            "input_drop=0 output_drop=0\r\n",
            "master",
        )
        with self.assertRaises(board_e2e.GateError):
            require_symbol(board_e2e, "_require_zero_drops")(
                "bridge_test stats input=0 output=0 injected=600 captured=600 "
                "input_drop=0 output_drop=1\r\n",
                "slave",
            )

    def test_readiness_matches_only_current_status_fields(self):
        board_e2e = load_board_e2e()
        master_ready = require_symbol(board_e2e, "_master_ready")
        slave_ready = require_symbol(board_e2e, "_slave_ready")
        self.assertTrue(master_ready("bridge_link active_count=1"))
        self.assertTrue(slave_ready("bridge_sync sync_state=2"))
        self.assertTrue(slave_ready("State:           LOCKED"))
        self.assertFalse(master_ready("active_count=10"))
        self.assertFalse(slave_ready("sync_state=20"))

    def test_readiness_requires_both_current_samples(self):
        board_e2e = load_board_e2e()
        current_pair_ready = require_symbol(board_e2e, "_current_pair_ready")
        self.assertFalse(
            current_pair_ready(
                "bridge_link active_count=1",
                "State:           ACQUIRING",
            )
        )
        self.assertFalse(
            current_pair_ready(
                "bridge_link active_count=0",
                "State:           LOCKED",
            )
        )
        self.assertTrue(
            current_pair_ready(
                "bridge_link active_count=1",
                "State:           LOCKED",
            )
        )

    def test_three_board_readiness_requires_mask_count_and_every_slave_locked(self):
        board_e2e = load_board_e2e()
        current_group_ready = require_symbol(board_e2e, "_current_group_ready")
        master = "bridge_link active_count=2 active_mask=0x03"

        self.assertTrue(
            current_group_ready(
                master,
                ["State:           LOCKED", "bridge_sync sync_state=2"],
                expected_mask=0x03,
            )
        )
        self.assertFalse(
            current_group_ready(
                "bridge_link active_count=1 active_mask=0x01",
                ["State:           LOCKED", "State:           LOCKED"],
                expected_mask=0x03,
            )
        )
        self.assertFalse(
            current_group_ready(
                master,
                ["State:           LOCKED", "State:           ACQUIRING"],
                expected_mask=0x03,
            )
        )

    def test_readiness_uses_latest_values_in_fresh_samples(self):
        board_e2e = load_board_e2e()
        current_pair_ready = require_symbol(board_e2e, "_current_pair_ready")
        self.assertFalse(
            current_pair_ready(
                "active_count=1\nactive_count=0",
                "State:           LOCKED",
            )
        )
        self.assertFalse(
            current_pair_ready(
                "active_count=1",
                "sync_state=2\nsync_state=0",
            )
        )
        self.assertFalse(
            current_pair_ready(
                "active_count=1",
                "State:           LOCKED\nState:           ACQUIRING",
            )
        )

    def test_command_completion_ignores_log_redraw_prompt_before_echo(self):
        board_e2e = load_board_e2e()
        complete = require_symbol(board_e2e, "_command_response_complete")
        redraw = b"bridge_sync sync_state=2\r\nuart:~$ "
        self.assertFalse(complete(redraw, "param get role_id"))
        response = redraw + b"param get role_id\r\nrole_id = 1 (persisted, reboot)\r\n"
        self.assertFalse(complete(response, "param get role_id"))
        self.assertTrue(complete(response + b"uart:~$ ", "param get role_id"))

    def test_runtime_drop_parser_accepts_all_zero_fields(self):
        board_e2e = load_board_e2e()
        require_runtime_zero = require_symbol(board_e2e, "_require_runtime_zero_drops")
        require_runtime_zero(
            "bridge_status uart_rx_drop_bytes=0 uart_tx_drop_bytes=0\r\n"
            "bridge_link queue_drop_bytes=0 radio_event_drop_count=0\r\n",
            "master",
        )

    def test_runtime_drop_parser_rejects_each_nonzero_field(self):
        board_e2e = load_board_e2e()
        require_runtime_zero = require_symbol(board_e2e, "_require_runtime_zero_drops")
        for field in (
            "uart_rx_drop_bytes",
            "uart_tx_drop_bytes",
            "queue_drop_bytes",
            "radio_event_drop_count",
        ):
            values = {
                "uart_rx_drop_bytes": 0,
                "uart_tx_drop_bytes": 0,
                "queue_drop_bytes": 0,
                "radio_event_drop_count": 0,
            }
            values[field] = 1
            output = (
                f"bridge_status uart_rx_drop_bytes={values['uart_rx_drop_bytes']} "
                f"uart_tx_drop_bytes={values['uart_tx_drop_bytes']}\r\n"
                f"bridge_link queue_drop_bytes={values['queue_drop_bytes']} "
                f"radio_event_drop_count={values['radio_event_drop_count']}\r\n"
            )
            with self.subTest(field=field), self.assertRaises(board_e2e.GateError):
                require_runtime_zero(output, "slave")

    def test_runtime_drop_parser_rejects_each_missing_field(self):
        board_e2e = load_board_e2e()
        require_runtime_zero = require_symbol(board_e2e, "_require_runtime_zero_drops")
        fields = (
            "uart_rx_drop_bytes=0",
            "uart_tx_drop_bytes=0",
            "queue_drop_bytes=0",
            "radio_event_drop_count=0",
        )
        for missing in fields:
            output = " ".join(field for field in fields if field != missing)
            with self.subTest(field=missing), self.assertRaises(board_e2e.GateError):
                require_runtime_zero(output, "master")

    def test_master_record_diagnostics_require_exact_bytes_and_idle_uart(self):
        board_e2e = load_board_e2e()
        require_record_diag = require_symbol(
            board_e2e, "_require_master_record_diagnostics"
        )
        output = (
            "node1_records=5 node1_bytes=600 node1_record_drop=0 "
            "uart_record_queued=5 uart_record_completed=5 "
            "uart_record_aborted=1 uart_record_rejected=0 "
            "uart_record_pending=0 uart_record_busy=0 uart_start_errors=0"
        )

        require_record_diag(output, expected_bytes=600, board="master")

        for replacement in (
            "node1_bytes=599",
            "node1_record_drop=1",
            "uart_record_completed=4",
            "uart_record_rejected=1",
            "uart_record_pending=1",
            "uart_record_busy=1",
            "uart_start_errors=1",
        ):
            broken = output
            field = replacement.split("=", 1)[0]
            broken = re.sub(rf"{field}=\d+", replacement, broken)
            with (
                self.subTest(replacement=replacement),
                self.assertRaises(board_e2e.GateError),
            ):
                require_record_diag(broken, expected_bytes=600, board="master")


class RuntimeFreshnessTests(unittest.TestCase):
    def test_bridge_stat_value_uses_latest_counter(self):
        board_e2e = load_board_e2e()
        bridge_stat_value = require_symbol(board_e2e, "_bridge_stat_value")

        self.assertEqual(
            bridge_stat_value(
                "downlink_gap=1 downlink_duplicate=0\n"
                "downlink_gap=3 downlink_duplicate=2",
                "downlink_gap",
            ),
            3,
        )

    def test_fresh_runtime_check_ignores_stale_nonzero_counters(self):
        board_e2e = load_board_e2e()
        wait_for_fresh = require_symbol(board_e2e, "_wait_for_fresh_runtime_zero_drops")

        class FakeBoard:
            label = "slave"
            command_timeout = 0.01

            def __init__(self):
                self.history = bytearray(
                    b"uart_rx_drop_bytes=4 uart_tx_drop_bytes=5 queue_drop_bytes=6 "
                    b"radio_event_drop_count=7"
                )
                self.commands = []
                self.events = []

            def history_bytes(self):
                return bytes(self.history)

            def history_mark(self):
                return len(self.history)

            def history_since(self, mark):
                return bytes(self.history[mark:])

            def command(self, command):
                self.commands.append(command)
                return (
                    "bridge_test stats uart_rx_drop_bytes=0 "
                    "uart_tx_drop_bytes=0 queue_drop_bytes=0 "
                    "radio_event_drop_count=0"
                )

            def event(self, message):
                self.events.append(message)

        board = FakeBoard()
        wait_for_fresh(board)
        self.assertEqual(board.commands, ["bridge_test stats"])
        self.assertEqual(
            board.events,
            [
                "runtime queue drops PASS uart_rx=0 uart_tx=0 "
                "bridge_queue=0 radio_event=0"
            ],
        )

    def test_fresh_runtime_check_ignores_stale_zero_counters(self):
        board_e2e = load_board_e2e()
        wait_for_fresh = require_symbol(board_e2e, "_wait_for_fresh_runtime_zero_drops")

        class FakeBoard:
            label = "master"
            command_timeout = 0.01

            def __init__(self):
                self.history = bytearray(
                    b"uart_rx_drop_bytes=0 uart_tx_drop_bytes=0 queue_drop_bytes=0 "
                    b"radio_event_drop_count=0"
                )

            def history_bytes(self):
                return bytes(self.history)

            def history_mark(self):
                return len(self.history)

            def history_since(self, mark):
                return bytes(self.history[mark:])

            def command(self, command):
                return (
                    "bridge_test stats uart_rx_drop_bytes=0 "
                    "uart_tx_drop_bytes=1 queue_drop_bytes=0 "
                    "radio_event_drop_count=0"
                )

            def event(self, message):
                pass

        with self.assertRaises(board_e2e.GateError):
            wait_for_fresh(FakeBoard())


class BestEffortBroadcastGateTests(unittest.TestCase):
    def test_retries_a_fresh_broadcast_after_one_slave_misses(self):
        board_e2e = load_board_e2e()
        run_gate = require_symbol(board_e2e, "_run_best_effort_broadcast_gate")

        class FakeBoard:
            command_timeout = 0.01

            def __init__(self, label, verify_results):
                self.label = label
                self.verify_results = iter(verify_results)
                self.commands = []
                self.events = []

            def command(self, command):
                self.commands.append(command)
                if command.startswith("bridge_test verify"):
                    return next(self.verify_results)
                if command == "bridge_test clear":
                    return "bridge_test clear ok"
                _, _, length, seed = command.split()
                return f"bridge_test inject_uart ok len={length} seed={seed}"

            def event(self, message):
                self.events.append(message)

        master = FakeBoard("master", [])
        slave1 = FakeBoard(
            "slave1",
            [
                "bridge_test verify pending need=32 available=0",
                "bridge_test verify ok len=32 seed=24",
            ],
        )
        slave2 = FakeBoard(
            "slave2",
            [
                "bridge_test verify ok len=32 seed=23",
                "bridge_test verify ok len=32 seed=24",
            ],
        )

        latency = run_gate(
            master,
            [slave1, slave2],
            32,
            23,
            "matrix-master-broadcast-32",
        )

        self.assertEqual(
            [command for command in master.commands if "inject_uart" in command],
            [
                "bridge_test inject_uart 32 23",
                "bridge_test inject_uart 32 24",
            ],
        )
        self.assertEqual(master.commands.count("bridge_test clear"), 2)
        self.assertEqual(slave1.commands.count("bridge_test clear"), 2)
        self.assertEqual(slave2.commands.count("bridge_test clear"), 2)
        self.assertEqual(set(latency), {"slave1", "slave2"})
        self.assertTrue(any("retry" in event for event in master.events))


class BootGuardClearTests(unittest.TestCase):
    def test_guard_clear_waits_until_minimum_uptime(self):
        board_e2e = load_board_e2e()
        wait_for_guard_clear = require_symbol(board_e2e, "_wait_for_boot_guard_clear")

        class FakeBoard:
            label = "master"
            command_timeout = 0.01

            def __init__(self):
                self.outputs = iter(
                    [
                        "Uptime: 9999 ms",
                        "Uptime: 10000 ms\nradio boot guard cleared after stable startup",
                    ]
                )
                self.commands = []
                self.events = []
                self.history = bytearray()

            def command(self, command):
                self.commands.append(command)
                output = next(self.outputs)
                self.history.extend(output.encode())
                return output

            def history_bytes(self):
                return bytes(self.history)

            def history_mark(self):
                return len(self.history)

            def history_since(self, mark):
                return bytes(self.history[mark:])

            def event(self, message):
                self.events.append(message)

        board = FakeBoard()
        with mock.patch.object(board_e2e.time, "sleep") as sleep:
            wait_for_guard_clear(board)

        self.assertEqual(board.commands, ["kernel uptime", "kernel uptime"])
        sleep.assert_called_once()
        self.assertEqual(
            board.events,
            ["boot guard clear PASS uptime_ms=10000 minimum_ms=10000"],
        )


class TimeUartFreshnessTests(unittest.TestCase):
    def test_fresh_time_uart_check_ignores_stale_error_counters(self):
        board_e2e = load_board_e2e()
        wait_for_fresh = require_symbol(
            board_e2e, "_wait_for_fresh_time_uart_zero_errors"
        )

        class FakeBoard:
            label = "slave"
            command_timeout = 0.01

            def __init__(self):
                self.history = bytearray(
                    b"time_uart rx_restart_errors=1 rx_buffer_errors=2"
                )
                self.commands = []
                self.events = []

            def history_mark(self):
                return len(self.history)

            def history_since(self, mark):
                return bytes(self.history[mark:])

            def command(self, command):
                self.commands.append(command)
                return (
                    "bridge_test time_uart rx_drop_bytes=0 "
                    "overlong_line_drops=0 output_line_drops=0 "
                    "rx_restart_errors=0 rx_buffer_errors=0 "
                    "rx_stopped_events=0 rx_stop_reason_mask=0 "
                    "pps_input_drop_count=0"
                )

            def event(self, message):
                self.events.append(message)

        board = FakeBoard()
        wait_for_fresh(board)

        self.assertEqual(board.commands, ["bridge_test stats"])
        self.assertEqual(
            board.events,
            ["time UART input errors and drops PASS"],
        )

    def test_time_uart_check_rejects_missing_reported_fields(self):
        board_e2e = load_board_e2e()
        wait_for_fresh = require_symbol(
            board_e2e, "_wait_for_fresh_time_uart_zero_errors"
        )

        class FakeBoard:
            label = "slave"
            command_timeout = 0.0

            def history_mark(self):
                return 0

            def history_since(self, mark):
                return b"time_uart rx_restart_errors=0 rx_buffer_errors=0"

            def command(self, command):
                return "bridge_test time_uart rx_restart_errors=0 rx_buffer_errors=0"

            def event(self, message):
                pass

        with self.assertRaises(board_e2e.GateError):
            wait_for_fresh(FakeBoard())

    def test_time_uart_check_rejects_each_nonzero_input_error_or_drop(self):
        board_e2e = load_board_e2e()
        wait_for_fresh = require_symbol(
            board_e2e, "_wait_for_fresh_time_uart_zero_errors"
        )
        fields = (
            "rx_drop_bytes",
            "overlong_line_drops",
            "output_line_drops",
            "rx_restart_errors",
            "rx_buffer_errors",
            "rx_stopped_events",
            "rx_stop_reason_mask",
            "pps_input_drop_count",
        )

        for failing_field in fields:
            with self.subTest(field=failing_field):
                values = {
                    "rx_drop_bytes": 0,
                    "overlong_line_drops": 0,
                    "output_line_drops": 0,
                    "rx_restart_errors": 0,
                    "rx_buffer_errors": 0,
                    "rx_stopped_events": 0,
                    "rx_stop_reason_mask": 0,
                    "pps_input_drop_count": 0,
                }
                values[failing_field] = 1
                status = (
                    "time_uart rx_drop_bytes={rx_drop_bytes} "
                    "overlong_line_drops={overlong_line_drops} "
                    "output_line_drops={output_line_drops} "
                    "rx_restart_errors={rx_restart_errors} "
                    "rx_buffer_errors={rx_buffer_errors} "
                    "rx_stopped_events={rx_stopped_events} "
                    "rx_stop_reason_mask={rx_stop_reason_mask} "
                    "pps_input_drop_count={pps_input_drop_count}"
                ).format(**values)

                class FakeBoard:
                    label = "master"
                    command_timeout = 0.0

                    def history_mark(self):
                        return 0

                    def history_since(self, mark):
                        return status.encode()

                    def command(self, command):
                        return status

                    def event(self, message):
                        pass

                with self.assertRaises(board_e2e.GateError):
                    wait_for_fresh(FakeBoard())


class ReadinessFreshnessTests(unittest.TestCase):
    def test_readiness_rejects_stale_ready_samples(self):
        board_e2e = load_board_e2e()
        wait_for_ready = require_symbol(board_e2e, "_wait_for_radio_ready")

        class FakeBoard:
            command_timeout = 0.0

            def __init__(self, label, stale, fresh, response):
                self.label = label
                self.history = bytearray(stale)
                self.fresh = fresh
                self.response = response

            def history_bytes(self):
                return bytes(self.history)

            def history_mark(self):
                return len(self.history)

            def history_since(self, mark):
                return bytes(self.history[mark:])

            def command(self, command):
                self.history.extend(self.fresh)
                return self.response

            def event(self, message):
                pass

        master = FakeBoard(
            "master",
            b"active_count=1",
            b" active_count=0",
            "Uptime: 10 ms",
        )
        slave = FakeBoard(
            "slave",
            b"sync_state=2 State: LOCKED",
            b" sync_state=0",
            "State:           ACQUIRING",
        )
        with (
            mock.patch.object(board_e2e, "STATUS_TIMEOUT_S", 1.0),
            mock.patch.object(board_e2e.time, "monotonic", side_effect=[0.0, 0.0, 2.0]),
            mock.patch.object(board_e2e.time, "sleep"),
        ):
            with self.assertRaises(board_e2e.GateError):
                wait_for_ready(master, slave)


class RoleManagementTests(unittest.TestCase):
    class FakeBoard:
        label = "master"

        def __init__(self, outputs):
            self.outputs = iter(outputs)
            self.commands = []
            self.events = []
            self.reboot_count = 0

        def command(self, command):
            self.commands.append(command)
            return next(self.outputs)

        def reboot(self):
            self.reboot_count += 1

        def event(self, message):
            self.events.append(message)

    def test_unchanged_role_does_not_reboot(self):
        board_e2e = load_board_e2e()
        ensure_role = require_symbol(board_e2e, "_ensure_role")
        board = self.FakeBoard(
            [
                "role_id = 0 (persisted, reboot)",
                "role_id = 0 (persisted, reboot)",
            ]
        )

        self.assertFalse(ensure_role(board, 0))
        self.assertEqual(
            board.commands,
            ["param get role_id", "param get role_id"],
        )
        self.assertEqual(board.reboot_count, 0)
        self.assertEqual(board.events, ["role_id=0 state=already-correct"])

    def test_changed_role_sets_value_and_cold_reboots(self):
        board_e2e = load_board_e2e()
        ensure_role = require_symbol(board_e2e, "_ensure_role")
        board = self.FakeBoard(
            [
                "role_id = 1 (persisted, reboot)",
                "role_id set to 0. Reboot required to apply.",
                "role_id = 0 (persisted, reboot)",
            ]
        )

        self.assertTrue(ensure_role(board, 0))
        self.assertEqual(
            board.commands,
            [
                "param get role_id",
                "param set role_id 0",
                "param get role_id",
            ],
        )
        self.assertEqual(board.reboot_count, 1)
        self.assertEqual(board.events, ["role_id=0 state=changed-and-rebooted"])


class WatchdogTriggerTests(unittest.TestCase):
    def test_require_waits_for_delayed_retained_watchdog_diagnostic(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave1",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        mark = board.history_mark()

        def receive_delayed_diagnostic(duration):
            self.assertEqual(duration, 0.1)
            board._remember(b"watchdog reset recovered: reset_reason=0x2\r\n")

        with (
            mock.patch.object(board, "_drain", side_effect=receive_delayed_diagnostic),
            mock.patch.object(board, "event") as event,
        ):
            board.require_watchdog_recovery(mark)

        event.assert_called_once_with("watchdog reset retained diagnostic PASS")

    def test_trigger_reconnects_and_requires_retained_watchdog_diagnostic(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave1",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )

        def reconnect_with_diagnostic(**unused):
            board._remember(b"watchdog reset recovered: reset_reason=0x2\r\n")

        with (
            mock.patch.object(board, "_drain"),
            mock.patch.object(board, "_write") as write,
            mock.patch.object(board, "close") as close,
            mock.patch.object(board, "connect", side_effect=reconnect_with_diagnostic),
            mock.patch.object(board, "event") as event,
            mock.patch.object(board_e2e.time, "sleep"),
        ):
            board.trigger_watchdog()

        write.assert_called_once_with(b"boot_test watchdog\r")
        close.assert_called_once_with()
        event.assert_any_call("watchdog reset retained diagnostic PASS")

    def test_trigger_fails_if_retained_watchdog_diagnostic_is_missing(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave1",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        with (
            mock.patch.object(board, "_drain"),
            mock.patch.object(board, "_write"),
            mock.patch.object(board, "close"),
            mock.patch.object(board, "connect"),
            mock.patch.object(board_e2e.time, "sleep"),
        ):
            with self.assertRaisesRegex(board_e2e.GateError, "watchdog diagnostic"):
                board.trigger_watchdog()
