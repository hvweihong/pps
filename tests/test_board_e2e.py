import importlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def load_board_e2e():
    try:
        return importlib.import_module("tools.board_e2e")
    except ModuleNotFoundError as exc:
        if exc.name == "tools.board_e2e":
            raise AssertionError("tools.board_e2e is not implemented") from exc
        raise


def require_symbol(module, name):
    try:
        return getattr(module, name)
    except AttributeError as exc:
        raise AssertionError(f"tools.board_e2e.{name} is not implemented") from exc


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
        (self.serial_by_id / (
            f"usb-Zephyr_Project_CDC_ACM_serial_backend_{device_id}0-if00"
        )).symlink_to(Path("/dev/ttyACM8"))

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

    def test_command_completion_ignores_log_redraw_prompt_before_echo(self):
        board_e2e = load_board_e2e()
        complete = require_symbol(board_e2e, "_command_response_complete")
        redraw = b"bridge_sync sync_state=2\r\nuart:~$ "
        self.assertFalse(complete(redraw, "param get role_id"))
        response = redraw + b"param get role_id\r\nrole_id = 1 (persisted, reboot)\r\n"
        self.assertFalse(complete(response, "param get role_id"))
        self.assertTrue(complete(response + b"uart:~$ ", "param get role_id"))


class CliTests(unittest.TestCase):
    def test_direct_script_execution_loads_tools_package(self):
        root = Path(__file__).resolve().parents[1]
        result = subprocess.run(
            [sys.executable, str(root / "tools" / "board_e2e.py"), "--help"],
            cwd=root,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--master-id", result.stdout)


if __name__ == "__main__":
    unittest.main()
