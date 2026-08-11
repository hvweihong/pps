import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tests.host.support import load_board_e2e, require_symbol


def make_three_board_args():
    return mock.Mock(
        master_id="DBE5C3D84EA2EC6F",
        slave_id="5B3D71D27A709CA2",
        slave2_id="DD01F9DF462D68A5",
        master_uf2=Path("firmware.uf2"),
        slave_uf2=Path("firmware.uf2"),
        slave2_uf2=Path("firmware.uf2"),
    )


class BoardPlanTests(unittest.TestCase):
    def test_second_slave_id_and_uf2_must_be_paired(self):
        board_e2e = load_board_e2e()
        args = make_three_board_args()
        args.slave2_uf2 = None

        with self.assertRaisesRegex(board_e2e.GateError, "must be provided together"):
            board_e2e._validate_board_plan(args)

    def test_three_board_ids_must_be_unique(self):
        board_e2e = load_board_e2e()
        args = make_three_board_args()
        args.slave2_id = args.slave_id

        with self.assertRaisesRegex(board_e2e.GateError, "must be distinct"):
            board_e2e._validate_board_plan(args)

    def test_three_board_plan_assigns_fixed_roles(self):
        board_e2e = load_board_e2e()
        plan = board_e2e._validate_board_plan(make_three_board_args())

        self.assertEqual(
            [(item.label, item.role_id) for item in plan],
            [("master", 0), ("slave1", 1), ("slave2", 2)],
        )

    def test_latency_summary_reports_min_median_p95_and_max(self):
        board_e2e = load_board_e2e()
        summary = board_e2e._latency_summary_ms([1.0, 2.0, 3.0, 20.0])

        self.assertEqual(summary["min"], 1.0)
        self.assertEqual(summary["median"], 2.5)
        self.assertEqual(summary["p95"], 20.0)
        self.assertEqual(summary["max"], 20.0)


class InjectionCommandTests(unittest.TestCase):
    def test_inject_pattern_selects_instant_or_uart_paced_command(self):
        board_e2e = load_board_e2e()
        board = mock.Mock(label="master")
        board.command.side_effect = lambda command: (
            f"bridge_test {'inject_uart' if 'inject_uart' in command else 'inject'} "
            "ok len=2048 seed=51"
        )

        board_e2e._inject_pattern(board, 2048, 51)
        board_e2e._inject_pattern(board, 2048, 51, paced=True)

        self.assertEqual(
            board.command.call_args_list,
            [
                mock.call("bridge_test inject 2048 51"),
                mock.call("bridge_test inject_uart 2048 51"),
            ],
        )

    def test_three_board_paths_select_uart_paced_injection(self):
        source = Path("tools/board_e2e.py").read_text(encoding="utf-8")
        broadcast = source[
            source.index("def _run_best_effort_broadcast_gate") :
            source.index("def _merge_latency_samples")
        ]
        smoke = source[
            source.index("def _run_three_board_smoke_gate") :
            source.index("def _run_bidirectional_bridge_gate")
        ]
        matrix = source[
            source.index("def _run_three_board_bridge_gate") :
            source.index("def _run_downlink_loss_gate")
        ]

        self.assertIn("_inject_pattern(master, length, attempt_seed, paced=True)", broadcast)
        self.assertIn("_run_best_effort_broadcast_gate(", smoke)
        self.assertIn("_run_best_effort_broadcast_gate(", matrix)
        self.assertEqual(smoke.count("paced=True"), 1)
        self.assertEqual(matrix.count("paced=True"), 3)


class ThreeBoardCycleTests(unittest.TestCase):
    def test_cycle_flashes_configures_and_runs_three_board_matrix(self):
        board_e2e = load_board_e2e()

        def fake_board(label):
            board = mock.Mock(label=label)
            board.command.side_effect = lambda command: (
                "Uptime: 12000 ms"
                if command == "kernel uptime"
                else "bridge_test clear ok"
            )
            return board

        master = fake_board("master")
        slave1 = fake_board("slave1")
        slave2 = fake_board("slave2")
        expected_latency = {"master_to_slave1": [1.25]}
        with (
            mock.patch.object(board_e2e, "_ensure_role") as ensure_role,
            mock.patch.object(board_e2e, "_wait_for_radio_ready") as wait_ready,
            mock.patch.object(
                board_e2e,
                "_run_three_board_bridge_gate",
                return_value=expected_latency,
            ) as bridge_gate,
        ):
            latency = board_e2e._run_cycle(
                master, [slave1, slave2], 1, 1, 600, 1
            )

        for board in (master, slave1, slave2):
            board.flash.assert_called_once_with()
            board.connect.assert_called_once_with()
            board.clear_history.assert_called_once_with()
        self.assertEqual(
            ensure_role.call_args_list,
            [mock.call(master, 0), mock.call(slave1, 1), mock.call(slave2, 2)],
        )
        wait_ready.assert_called_once_with(master, [slave1, slave2])
        bridge_gate.assert_called_once_with(master, [slave1, slave2])
        self.assertEqual(latency, expected_latency)


class RecoveryFlashTests(unittest.TestCase):
    def test_flash_counts_retry_attempt_when_both_attempts_fail(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        first_error = board_e2e.GateError("first flash failed")
        second_error = board_e2e.GateError("second flash failed")

        with (
            mock.patch.object(
                board,
                "_flash_once_for_current_state",
                side_effect=[first_error, second_error],
            ) as flash_once,
            mock.patch.object(board, "event"),
        ):
            with self.assertRaisesRegex(board_e2e.GateError, "second flash failed"):
                board.flash()

        self.assertEqual(flash_once.call_count, 2)
        self.assertEqual(board.flash_count, 0)
        self.assertEqual(board.flash_retry_count, 1)

    def test_recovery_guard_timeout_does_not_reenter_recovery(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        attempts = []

        def timeout_with_recursion_sentinel(command):
            attempts.append(command)
            if len(attempts) >= 3:
                raise RecursionError("boot guard recursively recovered")
            raise board_e2e.CommandTimeout(command, b"guard timeout")

        with (
            mock.patch.object(
                board, "_command_once", side_effect=timeout_with_recursion_sentinel
            ),
            mock.patch.object(
                board_e2e, "resolve_serial", return_value=Path("/dev/exact-board")
            ),
            mock.patch.object(board, "connect"),
            mock.patch.object(board, "event"),
            mock.patch.object(board_e2e.flash_uf2, "flash_once") as flash_once,
        ):
            with self.assertRaises(board_e2e.CommandTimeout):
                board.command("kernel uptime")

        self.assertEqual(attempts, ["kernel uptime", "kernel uptime"])
        self.assertEqual(board.recovery_count, 1)
        flash_once.assert_not_called()

    def test_flash_waits_for_application_identity_to_reenumerate(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        missing_serial = board_e2e.GateError("serial identity is missing")
        missing_disk = board_e2e.flash_uf2.FlashError("UF2 identity is missing")

        with (
            mock.patch.object(
                board_e2e,
                "resolve_serial",
                side_effect=[missing_serial, missing_serial, Path("/dev/exact-board")],
            ) as resolve,
            mock.patch.object(
                board_e2e, "_exact_application_identity_present", return_value=False
            ),
            mock.patch.object(
                board_e2e.flash_uf2,
                "uf2_disk_for",
                side_effect=[missing_disk, missing_disk],
            ),
            mock.patch.object(board, "connect"),
            mock.patch.object(board_e2e, "_wait_for_boot_guard_clear"),
            mock.patch.object(board_e2e.time, "sleep") as sleep,
            mock.patch.object(board_e2e.flash_uf2, "flash_once") as flash_once,
        ):
            board._flash_once_for_current_state()

        self.assertEqual(resolve.call_count, 3)
        self.assertEqual(sleep.call_count, 2)
        flash_once.assert_called_once_with(
            board.device_id,
            board.uf2,
            board_e2e.FLASH_TIMEOUT_S,
            no_1200_touch=False,
        )

    def test_flash_waits_for_uf2_identity_to_reenumerate(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        missing_serial = board_e2e.GateError("serial identity is missing")
        missing_disk = board_e2e.flash_uf2.FlashError("UF2 identity is missing")

        with (
            mock.patch.object(
                board_e2e, "resolve_serial", side_effect=missing_serial
            ) as resolve,
            mock.patch.object(
                board_e2e, "_exact_application_identity_present", return_value=False
            ),
            mock.patch.object(
                board_e2e.flash_uf2,
                "uf2_disk_for",
                side_effect=[missing_disk, Path("/dev/disk/by-id/exact-board")],
            ),
            mock.patch.object(board_e2e.time, "sleep") as sleep,
            mock.patch.object(board_e2e.flash_uf2, "flash_once") as flash_once,
        ):
            board._flash_once_for_current_state()

        self.assertEqual(resolve.call_count, 2)
        sleep.assert_called_once()
        flash_once.assert_called_once_with(
            board.device_id,
            board.uf2,
            board_e2e.FLASH_TIMEOUT_S,
            no_1200_touch=True,
        )

    def test_exact_identity_wait_fails_after_bounded_timeout(self):
        board_e2e = load_board_e2e()
        wait_for_identity = require_symbol(board_e2e, "_wait_for_exact_flash_identity")
        missing_serial = board_e2e.GateError("serial identity is missing")
        missing_disk = board_e2e.flash_uf2.FlashError("UF2 identity is missing")

        with (
            mock.patch.object(
                board_e2e, "resolve_serial", side_effect=missing_serial
            ) as resolve,
            mock.patch.object(
                board_e2e, "_exact_application_identity_present", return_value=False
            ),
            mock.patch.object(
                board_e2e.flash_uf2, "uf2_disk_for", side_effect=missing_disk
            ),
            mock.patch.object(board_e2e.time, "monotonic", side_effect=[0.0, 0.0, 2.0]),
            mock.patch.object(board_e2e.time, "sleep") as sleep,
        ):
            with self.assertRaisesRegex(board_e2e.GateError, "timed out"):
                wait_for_identity("DBE5C3D84EA2EC6F", 1.0)

        self.assertEqual(resolve.call_count, 2)
        sleep.assert_called_once()

    def test_application_flash_waits_for_boot_guard_clear(self):
        board_e2e = load_board_e2e()
        raw_log = mock.Mock()
        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            raw_log,
        )
        actions = []
        outputs = iter(
            [
                "Uptime: 9999 ms",
                "Uptime: 10000 ms",
            ]
        )

        def command(command_text):
            actions.append(command_text)
            output = next(outputs)
            board._remember(output.encode())
            return output

        with (
            mock.patch.object(
                board_e2e, "resolve_serial", return_value=Path("/dev/exact-board")
            ),
            mock.patch.object(
                board, "connect", side_effect=lambda: actions.append("connect")
            ),
            mock.patch.object(board, "_command_once", side_effect=command),
            mock.patch.object(board_e2e.time, "sleep"),
            mock.patch.object(
                board_e2e.flash_uf2,
                "flash_once",
                side_effect=lambda *args, **kwargs: actions.append("flash"),
            ) as flash_once,
        ):
            board._flash_once_for_current_state()

        self.assertEqual(
            actions,
            ["connect", "kernel uptime", "kernel uptime", "flash"],
        )
        flash_once.assert_called_once_with(
            board.device_id,
            board.uf2,
            board_e2e.FLASH_TIMEOUT_S,
            no_1200_touch=False,
        )

    def test_application_flash_recovers_when_shell_prompt_is_unavailable(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "slave",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        shell_error = board_e2e.GateError("shell readiness timed out")

        with (
            mock.patch.object(
                board_e2e, "resolve_serial", return_value=Path("/dev/exact-board")
            ),
            mock.patch.object(board, "connect", side_effect=shell_error),
            mock.patch.object(board, "event") as event,
            mock.patch.object(board_e2e.flash_uf2, "flash_once") as flash_once,
        ):
            board._flash_once_for_current_state()

        event.assert_called_once()
        self.assertIn("1200-baud recovery", event.call_args.args[0])
        flash_once.assert_called_once_with(
            board.device_id,
            board.uf2,
            board_e2e.FLASH_TIMEOUT_S,
            no_1200_touch=False,
        )

    def test_timeout_recovery_reflashes_reconnects_and_retries_command(self):
        board_e2e = load_board_e2e()
        raw_log = mock.Mock()
        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            raw_log,
        )
        timeout = board_e2e.CommandTimeout("kernel uptime", b"receive tail")
        with (
            mock.patch.object(
                board,
                "_command_once",
                side_effect=[timeout, "Uptime: 1000 ms"],
            ) as command_once,
            mock.patch.object(board, "_flash_once_for_current_state") as flash_once,
            mock.patch.object(board, "connect") as connect,
        ):
            output = board.command("kernel uptime")

        self.assertEqual(output, "Uptime: 1000 ms")
        self.assertEqual(command_once.call_count, 2)
        flash_once.assert_called_once_with()
        connect.assert_called_once_with()
        self.assertEqual(board.flash_count, 1)
        self.assertEqual(board.recovery_count, 1)

    def test_second_independent_timeout_exhausts_recovery_budget_before_flash(self):
        board_e2e = load_board_e2e()
        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            mock.Mock(),
        )
        first_timeout = board_e2e.CommandTimeout("kernel uptime", b"first")
        second_timeout = board_e2e.CommandTimeout("bridge_test stats", b"second")

        with (
            mock.patch.object(
                board,
                "_command_once",
                side_effect=[
                    first_timeout,
                    "Uptime: 10000 ms",
                    second_timeout,
                    "unexpected second retry",
                ],
            ),
            mock.patch.object(board, "_flash_once_for_current_state") as flash_once,
            mock.patch.object(board, "connect") as connect,
            mock.patch.object(board, "event"),
        ):
            self.assertEqual(board.command("kernel uptime"), "Uptime: 10000 ms")
            with self.assertRaisesRegex(board_e2e.GateError, "recovery budget"):
                board.command("bridge_test stats")

        flash_once.assert_called_once_with()
        connect.assert_called_once_with()
        self.assertEqual(board.flash_count, 1)
        self.assertEqual(board.recovery_count, 1)

    def test_timeout_recovery_makes_one_flash_attempt(self):
        board_e2e = load_board_e2e()

        class RecordingLog:
            def record(self, direction, payload):
                pass

        board = board_e2e.BoardSession(
            "master",
            "DBE5C3D84EA2EC6F",
            Path("firmware.uf2"),
            1.0,
            RecordingLog(),
        )
        flash_error = board_e2e.GateError("flash failed")
        with (
            mock.patch.object(
                board, "_flash_once_for_current_state", side_effect=flash_error
            ) as flash_once,
            mock.patch.object(board, "connect") as connect,
        ):
            with self.assertRaises(board_e2e.GateError):
                board.recover()

        flash_once.assert_called_once_with()
        connect.assert_not_called()

    def test_ambiguous_application_identity_never_falls_back_to_uf2(self):
        board_e2e = load_board_e2e()

        class RecordingLog:
            def record(self, direction, payload):
                pass

        board = board_e2e.BoardSession(
            "slave",
            "5B3D71D27A709CA2",
            Path("firmware.uf2"),
            1.0,
            RecordingLog(),
        )
        ambiguous = board_e2e.GateError("serial identity is missing or ambiguous")
        with (
            mock.patch.object(board_e2e, "resolve_serial", side_effect=ambiguous),
            mock.patch.object(
                board_e2e, "_exact_application_identity_present", return_value=True
            ),
            mock.patch.object(board_e2e.flash_uf2, "uf2_disk_for") as disk_for,
            mock.patch.object(board_e2e.flash_uf2, "flash_once") as flash_once,
        ):
            with self.assertRaises(board_e2e.GateError):
                board._flash_once_for_current_state()

        disk_for.assert_not_called()
        flash_once.assert_not_called()


class SteadyStateTests(unittest.TestCase):
    def test_steady_window_checks_both_boards_until_deadline(self):
        board_e2e = load_board_e2e()
        wait_for_steady = require_symbol(board_e2e, "_run_steady_state_window")

        class FakeBoard:
            def __init__(self, label):
                self.label = label
                self.events = []

            def event(self, message):
                self.events.append(message)

        master = FakeBoard("master")
        slave = FakeBoard("slave")
        with (
            mock.patch.object(board_e2e, "_wait_for_radio_ready") as ready,
            mock.patch.object(board_e2e, "_wait_for_fresh_runtime_zero_drops") as drops,
            mock.patch.object(
                board_e2e, "_wait_for_fresh_time_uart_zero_errors"
            ) as uart,
            mock.patch.object(
                board_e2e.time, "monotonic", side_effect=[0.0, 0.0, 0.0, 30.0]
            ),
            mock.patch.object(board_e2e.time, "sleep") as sleep,
        ):
            wait_for_steady(master, slave, 30.0)

        ready.assert_called_once_with(master, slave)
        self.assertEqual(drops.call_count, 2)
        self.assertEqual(uart.call_count, 2)
        sleep.assert_called_once_with(1.0)
        self.assertEqual(master.events, ["steady state PASS seconds=30"])
        self.assertEqual(slave.events, ["steady state PASS seconds=30"])


class RuntimeDropTests(unittest.TestCase):
    def test_runtime_drop_check_uses_on_demand_stats_command(self):
        board_e2e = load_board_e2e()

        class FakeBoard:
            label = "slave"
            queue_drops = 0

            def __init__(self):
                self.commands = []
                self.events = []

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
        board_e2e._wait_for_fresh_runtime_zero_drops(board)

        self.assertEqual(board.commands, ["bridge_test stats"])
        self.assertEqual(
            board.events,
            [
                "runtime queue drops PASS uart_rx=0 uart_tx=0 "
                "bridge_queue=0 radio_event=0"
            ],
        )

    def test_time_uart_check_uses_on_demand_stats_command(self):
        board_e2e = load_board_e2e()

        class FakeBoard:
            label = "slave"

            def __init__(self):
                self.commands = []
                self.events = []

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
        board_e2e._wait_for_fresh_time_uart_zero_errors(board)

        self.assertEqual(board.commands, ["bridge_test stats"])
        self.assertEqual(board.events, ["time UART input errors and drops PASS"])


class RawLogTests(unittest.TestCase):
    def test_raw_log_timestamps_and_flushes_each_record(self):
        board_e2e = load_board_e2e()
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "board.raw.log"
            with mock.patch.object(board_e2e.os, "fsync") as fsync:
                raw_log = board_e2e.TimestampedRawLog(path)
                raw_log.record("EVENT", b"cycle=1/1 PASS")
                raw_log.close()

            content = path.read_text()

        self.assertRegex(
            content,
            r"^\[\d{4}-\d{2}-\d{2}T.*Z\] EVENT cycle=1/1 PASS\n$",
        )
        fsync.assert_called_once()


class StageResultTests(unittest.TestCase):
    def valid_result(self):
        return {
            "master_to_slave": True,
            "slave_to_master": True,
            "queue_drops": 0,
            "recoveries": 0,
            "recovery_limit": 1,
        }

    def test_requires_both_bridge_directions(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["slave_to_master"] = False

        with self.assertRaises(board_e2e.GateError):
            validate(result)

    def test_rejects_queue_drop(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["queue_drops"] = 1

        with self.assertRaises(board_e2e.GateError):
            validate(result)

    def test_allows_one_recovery(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["recoveries"] = 1

        validate(result)

    def test_allows_one_recovery_per_board(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["recoveries"] = 2
        result["recovery_limit"] = 3

        validate(result)

    def test_rejects_recoveries_above_board_budget(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["recoveries"] = 4
        result["recovery_limit"] = 3

        with self.assertRaises(board_e2e.GateError):
            validate(result)


class RunnerEvidenceTests(unittest.TestCase):
    def _firmware_args(self, temp_root):
        master_uf2 = temp_root / "master.uf2"
        slave_uf2 = temp_root / "slave.uf2"
        master_uf2.write_bytes(b"master firmware")
        slave_uf2.write_bytes(b"slave firmware")
        return (
            master_uf2,
            slave_uf2,
            [
                "--stage",
                "stage8-host",
                "--master-uf2",
                str(master_uf2),
                "--slave-uf2",
                str(slave_uf2),
            ],
        )

    def _only_summary(self, evidence_root):
        evidence_dirs = list(evidence_root.iterdir())
        self.assertEqual(len(evidence_dirs), 1)
        summary_path = evidence_dirs[0] / "summary.json"
        self.assertTrue(summary_path.is_file())
        return json.loads(summary_path.read_text())

    def test_success_saves_logs_hashes_counts_and_validated_summary(self):
        board_e2e = load_board_e2e()
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            master_uf2, slave_uf2, args = self._firmware_args(temp_root)
            stdout = io.StringIO()
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(board_e2e, "_run_cycle"),
                mock.patch.object(
                    board_e2e,
                    "validate_stage_result",
                    wraps=board_e2e.validate_stage_result,
                ) as validate,
                mock.patch.object(sys, "stdout", stdout),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)
            raw_logs_exist = all(
                Path(path).is_file() for path in summary["raw_log_paths"].values()
            )

        self.assertEqual(return_code, 0)
        validate.assert_called_once()
        self.assertEqual(summary["stage"], "stage8-host")
        self.assertEqual(
            summary["board_ids"],
            {"master": board_e2e.MASTER_ID, "slave": board_e2e.SLAVE_ID},
        )
        self.assertEqual(
            summary["uf2_sha256"],
            {
                "master": hashlib.sha256(b"master firmware").hexdigest(),
                "slave": hashlib.sha256(b"slave firmware").hexdigest(),
            },
        )
        self.assertEqual(
            summary["directions"],
            {"master_to_slave": True, "slave_to_master": True},
        )
        self.assertEqual(summary["queue_drops"], 0)
        self.assertEqual(
            summary["boards"]["master"],
            {"flashes": 0, "flash_retries": 0, "recoveries": 0},
        )
        self.assertTrue(summary["pass"])
        self.assertTrue(summary["complete"])
        self.assertIsNone(summary["error"])
        self.assertTrue(raw_logs_exist)
        self.assertRegex(summary["started_at"], r"Z$")
        self.assertRegex(summary["ended_at"], r"Z$")
        self.assertIn("watchdog_restarts_per_board=0", stdout.getvalue())

    def test_three_board_success_saves_each_board_log_and_summary(self):
        board_e2e = load_board_e2e()
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            master_uf2, slave_uf2, args = self._firmware_args(temp_root)
            slave2_uf2 = temp_root / "slave2.uf2"
            slave2_uf2.write_bytes(b"slave2 firmware")
            args.extend(
                [
                    "--slave2-id",
                    "DD01F9DF462D68A5",
                    "--slave2-uf2",
                    str(slave2_uf2),
                ]
            )
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e,
                    "_run_cycle",
                    return_value={
                        "master_to_slave1": [1.0, 2.0],
                        "slave1_to_master": [3.0],
                    },
                ),
                mock.patch.object(sys, "stdout", io.StringIO()),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 0)
        self.assertEqual(
            summary["board_ids"],
            {
                "master": board_e2e.MASTER_ID,
                "slave1": board_e2e.SLAVE_ID,
                "slave2": "DD01F9DF462D68A5",
            },
        )
        self.assertEqual(
            set(summary["raw_log_paths"]), {"master", "slave1", "slave2"}
        )
        self.assertEqual(set(summary["boards"]), {"master", "slave1", "slave2"})
        self.assertEqual(
            summary["latency_ms"]["samples"]["master_to_slave1"], [1.0, 2.0]
        )
        self.assertEqual(
            summary["latency_ms"]["summary"]["master_to_slave1"],
            {"min": 1.0, "median": 1.5, "p95": 2.0, "max": 2.0},
        )

    def test_failure_saves_incomplete_summary_and_known_counts(self):
        board_e2e = load_board_e2e()

        def fail_after_counts(master, slave, *unused):
            master.flash_count = 2
            master.flash_retry_count = 1
            master.recovery_count = 1
            slave.flash_count = 1
            raise board_e2e.GateError("simulated stage failure")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            stderr = io.StringIO()
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e, "_run_cycle", side_effect=fail_after_counts
                ),
                mock.patch.object(sys, "stderr", stderr),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 1)
        self.assertFalse(summary["pass"])
        self.assertFalse(summary["complete"])
        self.assertEqual(summary["error"], "simulated stage failure")
        self.assertEqual(
            summary["boards"]["master"],
            {"flashes": 2, "flash_retries": 1, "recoveries": 1},
        )
        self.assertEqual(
            summary["boards"]["slave"],
            {"flashes": 1, "flash_retries": 0, "recoveries": 0},
        )
        self.assertEqual(
            summary["directions"],
            {"master_to_slave": False, "slave_to_master": False},
        )
        self.assertIn("BOARD_E2E FAIL", stderr.getvalue())

    def test_unexpected_exception_saves_type_and_message(self):
        board_e2e = load_board_e2e()
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            stderr = io.StringIO()
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e,
                    "_run_cycle",
                    side_effect=RuntimeError("unexpected stage crash"),
                ),
                mock.patch.object(sys, "stderr", stderr),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 1)
        self.assertFalse(summary["pass"])
        self.assertFalse(summary["complete"])
        self.assertIn("RuntimeError: unexpected stage crash", summary["error"])
        self.assertIn("RuntimeError: unexpected stage crash", stderr.getvalue())

    def test_cleanup_failures_do_not_block_remaining_cleanup_or_summary(self):
        board_e2e = load_board_e2e()
        master_log = mock.Mock()
        master_log.close.side_effect = RuntimeError("master raw log close failed")
        slave_log = mock.Mock()
        sessions = {}

        def install_cleanup_failures(master, slave, *unused):
            sessions["master"] = master
            sessions["slave"] = slave
            master.close = mock.Mock(
                side_effect=RuntimeError("master board close failed")
            )
            slave.close = mock.Mock()

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e,
                    "TimestampedRawLog",
                    side_effect=[master_log, slave_log],
                ),
                mock.patch.object(
                    board_e2e, "_run_cycle", side_effect=install_cleanup_failures
                ),
                mock.patch.object(sys, "stderr", io.StringIO()),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 1)
        self.assertFalse(summary["pass"])
        self.assertFalse(summary["complete"])
        self.assertIn("cleanup master board", summary["error"])
        self.assertIn("RuntimeError: master board close failed", summary["error"])
        self.assertIn("cleanup master raw log", summary["error"])
        self.assertIn("RuntimeError: master raw log close failed", summary["error"])
        sessions["master"].close.assert_called_once_with()
        sessions["slave"].close.assert_called_once_with()
        master_log.close.assert_called_once_with()
        slave_log.close.assert_called_once_with()

    def test_summary_write_failure_is_not_reported_as_pass(self):
        board_e2e = load_board_e2e()
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(board_e2e, "_run_cycle"),
                mock.patch.object(
                    board_e2e,
                    "_write_json_summary",
                    side_effect=OSError("summary disk full"),
                ),
                mock.patch.object(sys, "stdout", stdout),
                mock.patch.object(sys, "stderr", stderr),
            ):
                return_code = board_e2e.main(args)

        self.assertEqual(return_code, 1)
        self.assertNotIn("BOARD_E2E PASS", stdout.getvalue())
        self.assertIn("summary write failed", stderr.getvalue())
        self.assertIn("OSError: summary disk full", stderr.getvalue())

    def test_failure_summary_records_observed_queue_drops(self):
        board_e2e = load_board_e2e()
        require_zero_drops = require_symbol(board_e2e, "_require_zero_drops")

        def fail_on_observed_drops(master, slave, *unused):
            require_zero_drops(
                "bridge_test stats input_drop=2 output_drop=3",
                master,
            )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e, "_run_cycle", side_effect=fail_on_observed_drops
                ),
                mock.patch.object(sys, "stderr", io.StringIO()),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 1)
        self.assertEqual(summary["queue_drops"], 5)
        self.assertIn("input_drop=2 output_drop=3", summary["error"])

    def test_each_board_has_an_independent_recovery_budget(self):
        board_e2e = load_board_e2e()
        recovery_flashes = {}

        def recover_both_boards(master, slave, *unused):
            for board in (master, slave):
                recovery_flashes[board.label] = mock.Mock()
                board._flash_once_for_current_state = recovery_flashes[board.label]
                board.connect = mock.Mock()
            master.recover()
            slave.recover()
            raise board_e2e.GateError("simulated stage failure")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            evidence_root = temp_root / "evidence"
            _, _, args = self._firmware_args(temp_root)
            with (
                mock.patch.object(board_e2e, "EVIDENCE_ROOT", evidence_root),
                mock.patch.object(
                    board_e2e, "_run_cycle", side_effect=recover_both_boards
                ),
                mock.patch.object(sys, "stderr", io.StringIO()),
            ):
                return_code = board_e2e.main(args)

            summary = self._only_summary(evidence_root)

        self.assertEqual(return_code, 1)
        recovery_flashes["master"].assert_called_once_with()
        recovery_flashes["slave"].assert_called_once_with()
        self.assertEqual(summary["recoveries"], 2)
        self.assertIn("simulated stage failure", summary["error"])


class ThreeBoardRestartTests(unittest.TestCase):
    def fake_board(self, label):
        board = mock.Mock(label=label, command_timeout=1.0, queue_drops=0)
        board.recovery_count = 0
        board.flash_count = 0
        board.flash_retry_count = 0
        return board

    def test_cold_restart_is_sequential_and_observes_reduced_masks(self):
        board_e2e = load_board_e2e()
        master = self.fake_board("master")
        slave1 = self.fake_board("slave1")
        slave2 = self.fake_board("slave2")
        with (
            mock.patch.object(
                board_e2e,
                "_uptime_ms",
                side_effect=[12000, 100, 13000, 120, 14000, 140],
            ),
            mock.patch.object(board_e2e, "_wait_for_boot_guard_clear"),
            mock.patch.object(board_e2e, "_wait_for_active_mask") as wait_mask,
            mock.patch.object(board_e2e, "_wait_for_radio_ready") as wait_ready,
            mock.patch.object(board_e2e, "_wait_for_fresh_runtime_zero_drops"),
            mock.patch.object(board_e2e, "_wait_for_fresh_time_uart_zero_errors"),
            mock.patch.object(board_e2e, "_run_three_board_smoke_gate") as smoke,
            mock.patch.object(board_e2e.time, "sleep"),
        ):
            board_e2e._run_cold_reboot_cycles(
                master, [slave1, slave2], 1, bridge_length=600
            )

        for board in (master, slave1, slave2):
            board.reboot_and_disconnect.assert_called_once_with()
            board.connect.assert_called_once_with()
            board.flash.assert_not_called()
        self.assertEqual(
            wait_mask.call_args_list,
            [mock.call(master, 0x02), mock.call(master, 0x01)],
        )
        self.assertEqual(wait_ready.call_count, 3)
        self.assertEqual(smoke.call_count, 3)

    def test_watchdog_restart_checks_each_board_without_touching_peers(self):
        board_e2e = load_board_e2e()
        master = self.fake_board("master")
        slave1 = self.fake_board("slave1")
        slave2 = self.fake_board("slave2")
        with (
            mock.patch.object(board_e2e, "_wait_for_active_mask") as wait_mask,
            mock.patch.object(board_e2e, "_wait_for_radio_ready") as wait_ready,
            mock.patch.object(board_e2e, "_wait_for_boot_guard_clear"),
            mock.patch.object(board_e2e, "_wait_for_fresh_runtime_zero_drops"),
            mock.patch.object(board_e2e, "_wait_for_fresh_time_uart_zero_errors"),
            mock.patch.object(board_e2e, "_run_three_board_smoke_gate") as smoke,
        ):
            board_e2e._run_watchdog_reboot_cycles(
                master, [slave1, slave2], cycles=1, bridge_length=600
            )

        for board in (master, slave1, slave2):
            board.watchdog_reset_begin.assert_called_once_with()
            board.connect.assert_called_once_with(wait_timeout=40.0)
            board.require_watchdog_recovery.assert_called_once_with(
                board.watchdog_reset_begin.return_value
            )
            board.flash.assert_not_called()
            board.reboot.assert_not_called()
        self.assertEqual(
            wait_mask.call_args_list,
            [mock.call(master, 0x02), mock.call(master, 0x01)],
        )
        self.assertEqual(wait_ready.call_count, 3)
        self.assertEqual(smoke.call_count, 3)


class CliTests(unittest.TestCase):
    def test_watchdog_restarts_option_is_parsed(self):
        board_e2e = load_board_e2e()
        args = board_e2e.build_parser().parse_args(
            [
                "--stage",
                "restart",
                "--master-uf2",
                "master.uf2",
                "--slave-uf2",
                "slave.uf2",
                "--watchdog-restarts-per-board",
                "1",
            ]
        )

        self.assertEqual(args.watchdog_restarts_per_board, 1)

    def test_bridge_repeats_option_is_parsed(self):
        board_e2e = load_board_e2e()
        args = board_e2e.build_parser().parse_args(
            [
                "--stage",
                "stage6",
                "--master-uf2",
                "master.uf2",
                "--slave-uf2",
                "slave.uf2",
                "--bridge-repeats",
                "5",
            ]
        )

        self.assertEqual(args.bridge_repeats, 5)

    def test_uplink_ack_loss_once_option_is_parsed(self):
        board_e2e = load_board_e2e()
        args = board_e2e.build_parser().parse_args(
            [
                "--stage",
                "stage6",
                "--master-uf2",
                "master.uf2",
                "--slave-uf2",
                "slave.uf2",
                "--uplink-ack-loss-once",
            ]
        )

        self.assertTrue(args.uplink_ack_loss_once)

    def test_downlink_loss_cadence_option_is_parsed(self):
        board_e2e = load_board_e2e()
        args = board_e2e.build_parser().parse_args(
            [
                "--stage",
                "stage5",
                "--master-uf2",
                "master.uf2",
                "--slave-uf2",
                "slave.uf2",
                "--downlink-loss-every-n",
                "3",
            ]
        )

        self.assertEqual(args.downlink_loss_every_n, 3)

    def test_bridge_length_above_firmware_capacity_fails(self):
        board_e2e = load_board_e2e()
        stderr = io.StringIO()
        with mock.patch.object(sys, "stderr", stderr):
            result = board_e2e.main(
                [
                    "--stage",
                    "stage1-baseline",
                    "--master-uf2",
                    "missing-master.uf2",
                    "--slave-uf2",
                    "missing-slave.uf2",
                    "--bridge-length",
                    "2049",
                ]
            )

        self.assertEqual(result, 1)
        self.assertIn("--bridge-length must be in [1, 2048]", stderr.getvalue())

    def test_negative_steady_state_seconds_fails(self):
        board_e2e = load_board_e2e()
        stderr = io.StringIO()
        with mock.patch.object(sys, "stderr", stderr):
            result = board_e2e.main(
                [
                    "--stage",
                    "stage3",
                    "--master-uf2",
                    "missing-master.uf2",
                    "--slave-uf2",
                    "missing-slave.uf2",
                    "--steady-state-seconds",
                    "-1",
                ]
            )

        self.assertEqual(result, 1)
        self.assertIn("--steady-state-seconds must be >= 0", stderr.getvalue())

    def test_direct_script_execution_loads_tools_package(self):
        root = Path(__file__).resolve().parents[2]
        result = subprocess.run(
            [sys.executable, str(root / "tools" / "board_e2e.py"), "--help"],
            cwd=root,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--master-id", result.stdout)
