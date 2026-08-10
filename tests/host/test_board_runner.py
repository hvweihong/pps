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

    def test_rejects_two_recoveries(self):
        board_e2e = load_board_e2e()
        validate = require_symbol(board_e2e, "validate_stage_result")
        result = self.valid_result()
        result["recoveries"] = 2

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

    def test_second_board_recovery_is_rejected_before_reflash(self):
        board_e2e = load_board_e2e()
        recovery_flashes = {}

        def recover_both_boards(master, slave, *unused):
            for board in (master, slave):
                recovery_flashes[board.label] = mock.Mock()
                board._flash_once_for_current_state = recovery_flashes[board.label]
                board.connect = mock.Mock()
            master.recover()
            slave.recover()

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
        recovery_flashes["slave"].assert_not_called()
        self.assertEqual(summary["recoveries"], 1)
        self.assertIn("slave: automatic recovery budget exhausted", summary["error"])


class CliTests(unittest.TestCase):
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
