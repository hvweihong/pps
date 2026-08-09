import errno
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import flash_uf2


class HostCommandTests(unittest.TestCase):
    def test_host_commands_use_a_bounded_subprocess_timeout(self):
        commands = (
            ("findmnt", "-rn", "-S", "/dev/sda", "-o", "TARGET"),
            ("lsblk", "-dn", "-o", "LABEL", "/dev/sda"),
            (
                "udisksctl",
                "mount",
                "-b",
                "/dev/sda",
                "--no-user-interaction",
            ),
        )
        for command in commands:
            completed = subprocess.CompletedProcess(command, 0, stdout="")
            with self.subTest(command=command), mock.patch.object(
                flash_uf2.subprocess, "run", return_value=completed
            ) as run:
                flash_uf2._run(*command)

            timeout = run.call_args.kwargs.get("timeout")
            self.assertIsNotNone(timeout)
            self.assertGreater(timeout, 0)

    def test_host_command_timeout_is_reported_as_flash_error(self):
        command = ("findmnt", "-rn", "-S", "/dev/sda", "-o", "TARGET")
        with mock.patch.object(
            flash_uf2.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(command, 1.0),
        ):
            with self.assertRaisesRegex(
                flash_uf2.FlashError, "command timed out"
            ):
                flash_uf2._run(*command)


class FlashDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.serial = root / "serial" / "by-id"
        self.disk = root / "disk" / "by-id"
        self.serial.mkdir(parents=True)
        self.disk.mkdir(parents=True)
        self.addCleanup(self.tmp.cleanup)

    def test_serial_id_is_exact_and_resolves_symlink(self):
        requested = "DBE5C3D84EA2EC6F"
        target = Path("/dev/ttyACM7")
        link = self.serial / f"usb-Seeed_XIAO_nRF52840_Plus_{requested}-if00"
        link.symlink_to(target)
        (self.serial / f"usb-Seeed_XIAO_nRF52840_Plus_{requested}0-if00").symlink_to(
            Path("/dev/ttyACM8")
        )

        self.assertEqual(flash_uf2.serial_port_for(requested, self.serial), link)

    def test_application_zephyr_serial_prefix_is_supported(self):
        requested = "DBE5C3D84EA2EC6F"
        link = self.serial / (
            f"usb-Zephyr_Project_CDC_ACM_serial_backend_{requested}-if00"
        )
        link.symlink_to(Path("/dev/ttyACM7"))
        self.assertEqual(flash_uf2.serial_port_for(requested, self.serial), link)

    def test_missing_or_ambiguous_serial_fails_closed(self):
        requested = "DBE5C3D84EA2EC6F"
        with self.assertRaises(flash_uf2.FlashError):
            flash_uf2.serial_port_for(requested, self.serial)

        for index in (0, 1):
            (self.serial / f"usb-Seeed_XIAO_nRF52840_Plus_{requested}-if0{index}").symlink_to(
                Path(f"/dev/ttyACM{index}")
            )
        with self.assertRaises(flash_uf2.FlashError):
            flash_uf2.serial_port_for(requested, self.serial)

    def test_uf2_disk_uses_complete_id_and_expected_name(self):
        requested = "DBE5C3D84EA2EC6F"
        disk = self.disk / f"usb-Adafruit_nRF_UF2_{requested}-0:0"
        disk.symlink_to(Path("/dev/sda"))
        other = self.disk / "usb-Adafruit_nRF_UF2_DBE5C3D84EA2EC6F0-0:0"
        other.symlink_to(Path("/dev/sdb"))

        self.assertEqual(flash_uf2.uf2_disk_for(requested, self.disk), disk)
        self.assertEqual(flash_uf2.uf2_disk_name(requested), disk.name)

    def test_missing_or_ambiguous_disk_fails_closed(self):
        requested = "DBE5C3D84EA2EC6F"
        with self.assertRaises(flash_uf2.FlashError):
            flash_uf2.uf2_disk_for(requested, self.disk)
        for name, target in (
            (f"usb-Adafruit_nRF_UF2_{requested}-0:0", "/dev/sda"),
            (f"usb-Adafruit_nRF_UF2_{requested}-1:0", "/dev/sdb"),
        ):
            (self.disk / name).symlink_to(Path(target))
        with self.assertRaises(flash_uf2.FlashError):
            flash_uf2.uf2_disk_for(requested, self.disk)

    def test_mounted_root_requires_label_and_info_file(self):
        mount = Path(self.tmp.name) / "mount"
        mount.mkdir()
        (mount / "INFO_UF2.TXT").write_text("UF2 family: nRF52840\n")
        disk = Path("/dev/sda")

        with mock.patch.object(flash_uf2, "find_mount", return_value=mount), \
             mock.patch.object(flash_uf2, "block_label", return_value="XIAO-BOOT"):
            self.assertEqual(flash_uf2.mounted_uf2_root(disk), mount)

        with mock.patch.object(flash_uf2, "find_mount", return_value=mount), \
             mock.patch.object(flash_uf2, "block_label", return_value="OTHER"):
            with self.assertRaises(flash_uf2.FlashError):
                flash_uf2.mounted_uf2_root(disk)

        (mount / "INFO_UF2.TXT").unlink()
        with mock.patch.object(flash_uf2, "find_mount", return_value=mount), \
             mock.patch.object(flash_uf2, "block_label", return_value="XIAO-BOOT"):
            with self.assertRaises(flash_uf2.FlashError):
                flash_uf2.mounted_uf2_root(disk)

    def test_wait_for_mount_requests_desktop_mount_once(self):
        disk = Path("/dev/sda")
        mount = Path(self.tmp.name) / "mount-requested"
        mount.mkdir()
        missing = flash_uf2.FlashError("UF2 disk is not mounted")

        with mock.patch.object(flash_uf2, "find_mount",
                               side_effect=[missing, mount]), \
             mock.patch.object(flash_uf2, "_run") as run:
            self.assertEqual(flash_uf2.wait_for_mount(disk, 1.0), mount)

        run.assert_called_once_with(
            "udisksctl", "mount", "-b", str(disk), "--no-user-interaction"
        )


class FlashCopyTests(unittest.TestCase):
    def test_copy_writes_complete_source_and_syncs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "firmware.uf2"
            payload = os.urandom(4097)
            source.write_bytes(payload)
            destination = root / "mount"
            destination.mkdir()

            with mock.patch.object(flash_uf2, "sync_filesystem") as sync:
                written = flash_uf2.copy_uf2(source, destination)

            self.assertTrue(written.name.endswith(".uf2"))
            self.assertEqual(written.read_bytes(), payload)
            sync.assert_called_once()


class FlashTouchTests(unittest.TestCase):
    def test_1200_touch_accepts_disconnect_during_reboot(self):
        class RebootingSerial:
            def __init__(self, *args, **kwargs):
                pass

            def __enter__(self):
                raise OSError(errno.EIO, "device disconnected during reboot")

            def __exit__(self, *args):
                return False

        with mock.patch.object(flash_uf2, "serial", mock.Mock(Serial=RebootingSerial)):
            flash_uf2.touch_1200(Path("/dev/ttyACM0"))


if __name__ == "__main__":
    unittest.main()
