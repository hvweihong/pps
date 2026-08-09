#!/usr/bin/env python3
"""Serial-number-bound UF2 flasher for XIAO nRF52840 boards.

The bootloader is treated as a block device: a completed UF2 copy is the
commit point, and each subsequent wait is bound to the same full USB ID.
"""

from __future__ import annotations

import argparse
import errno
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Iterable

try:
    import serial
except ImportError:  # pragma: no cover - only needed for hardware operation
    serial = None


DEFAULT_SERIAL_BY_ID = Path("/dev/serial/by-id")
DEFAULT_DISK_BY_ID = Path("/dev/disk/by-id")
SERIAL_PREFIX = "usb-Seeed_XIAO_nRF52840_Plus_"
# The bootloader exposes the board as Seeed USB, while the application built
# by this project identifies itself as Zephyr CDC ACM.  Both names retain the
# same full USB serial suffix and are accepted; the suffix remains the only
# board-selection key.
SERIAL_PREFIXES = (
    SERIAL_PREFIX,
    "usb-Zephyr_Project_CDC_ACM_serial_backend_",
)
DISK_PREFIX = "usb-Adafruit_nRF_UF2_"
UF2_LABEL = "XIAO-BOOT"


class FlashError(RuntimeError):
    """A fail-closed discovery, validation, or flashing error."""


def _validate_id(device_id: str) -> str:
    if not device_id or len(device_id) < 8 or any(c not in "0123456789abcdefABCDEF" for c in device_id):
        raise FlashError(f"invalid complete USB device ID: {device_id!r}")
    return device_id.upper()


def serial_link_name(device_id: str) -> str:
    return f"{SERIAL_PREFIX}{_validate_id(device_id)}-if00"


def uf2_disk_name(device_id: str) -> str:
    return f"{DISK_PREFIX}{_validate_id(device_id)}-0:0"


def _one_link(directory: Path, name: str, kind: str) -> Path:
    expected = directory / name
    if not directory.exists():
        raise FlashError(f"{kind} identity directory is missing: {directory}")
    matches = [entry for entry in directory.iterdir() if entry.name == name]
    if name.endswith("-if00"):
        related = [entry for entry in directory.iterdir()
                   if entry.name.startswith(name[:-5] + "-if")]
    elif name.endswith("-0:0"):
        related = [entry for entry in directory.iterdir()
                   if entry.name.startswith(name[:-4] + "-")]
    else:
        related = matches
    if len(matches) != 1 or len(related) != 1 or not matches[0].is_symlink():
        raise FlashError(f"{kind} identity is missing or ambiguous: {expected}")
    return matches[0]


def serial_port_for(device_id: str, serial_by_id: Path = DEFAULT_SERIAL_BY_ID) -> Path:
    device_id = _validate_id(device_id)
    if not serial_by_id.exists():
        raise FlashError(f"serial identity directory is missing: {serial_by_id}")
    matches: list[Path] = []
    for prefix in SERIAL_PREFIXES:
        name = f"{prefix}{device_id}-if00"
        try:
            matches.append(_one_link(serial_by_id, name, "serial"))
        except FlashError:
            continue
    if len(matches) != 1:
        raise FlashError(f"serial identity is missing or ambiguous for {device_id}")
    return matches[0]


def uf2_disk_for(device_id: str, disk_by_id: Path = DEFAULT_DISK_BY_ID) -> Path:
    return _one_link(disk_by_id, uf2_disk_name(device_id), "UF2 disk")


def _run(*args: str) -> str:
    try:
        result = subprocess.run(args, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise FlashError(f"command failed: {' '.join(args)}: {exc}") from exc
    return result.stdout.strip()


def find_mount(disk: Path) -> Path:
    output = _run("findmnt", "-rn", "-S", str(disk), "-o", "TARGET")
    if not output:
        raise FlashError(f"UF2 disk is not mounted: {disk}")
    mounts = [Path(line) for line in output.splitlines() if line]
    if len(mounts) != 1:
        raise FlashError(f"UF2 disk mount is missing or ambiguous: {disk}")
    return mounts[0]


def block_label(disk: Path) -> str:
    return _run("lsblk", "-dn", "-o", "LABEL", str(disk)).strip()


def wait_for_mount(disk: Path, timeout_s: float) -> Path:
    deadline = time.monotonic() + timeout_s
    last_error: FlashError | None = None
    mount_requested = False
    while time.monotonic() < deadline:
        try:
            return find_mount(disk)
        except FlashError as exc:
            last_error = exc
            if not mount_requested:
                mount_requested = True
                try:
                    _run("udisksctl", "mount", "-b", str(disk),
                         "--no-user-interaction")
                except FlashError:
                    # The desktop policy may already have mounted the volume,
                    # or may reject the request; polling remains authoritative.
                    pass
            time.sleep(0.1)
    raise FlashError(f"timed out waiting for {disk} to be mounted") from last_error


def mounted_uf2_root(disk: Path, timeout_s: float = 10.0) -> Path:
    if block_label(disk) != UF2_LABEL:
        raise FlashError(f"unexpected UF2 label for {disk}: {block_label(disk)!r}")
    mount = wait_for_mount(disk, timeout_s)
    if not mount.is_dir():
        raise FlashError(f"UF2 mount is not a directory: {mount}")
    if not (mount / "INFO_UF2.TXT").is_file():
        raise FlashError(f"INFO_UF2.TXT is missing from UF2 mount: {mount}")
    return mount


def wait_for_link(device_id: str, directory: Path, timeout_s: float, *, disk: bool) -> Path:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            return uf2_disk_for(device_id, directory) if disk else serial_port_for(device_id, directory)
        except FlashError:
            time.sleep(0.05)
    what = "UF2 disk" if disk else "application serial"
    raise FlashError(f"timed out waiting for {what} for device {device_id}")


def wait_for_absence(device_id: str, directory: Path, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            uf2_disk_for(device_id, directory)
        except FlashError:
            return
        time.sleep(0.05)
    raise FlashError(f"timed out waiting for UF2 disk to disappear for device {device_id}")


def touch_1200(serial_port: Path) -> None:
    if serial is None:
        raise FlashError("pyserial is required for 1200-baud bootloader touch")
    try:
        with serial.Serial(str(serial_port.resolve()), baudrate=1200, timeout=0, write_timeout=0):
            pass
    except Exception as exc:  # pyserial has platform-specific exception classes
        # The application resets as soon as the CDC line coding is delivered.
        # Linux can report the expected unplug during context-manager close as
        # EIO/ENODEV; the following UF2 enumeration is the commit-point check.
        if getattr(exc, "errno", None) in (errno.EIO, errno.ENODEV):
            return
        raise FlashError(f"1200-baud touch failed for {serial_port}: {exc}") from exc


def sync_filesystem() -> None:
    try:
        os.sync()
    except AttributeError:  # pragma: no cover - non-POSIX host
        _run("sync")


def copy_uf2(source: Path, destination_root: Path) -> Path:
    if not source.is_file():
        raise FlashError(f"UF2 source does not exist: {source}")
    if not destination_root.is_dir():
        raise FlashError(f"UF2 destination is not a directory: {destination_root}")
    destination = destination_root / f"firmware-{uuid.uuid4().hex}.uf2"
    source_size = source.stat().st_size
    bytes_written = 0
    try:
        with source.open("rb") as src, destination.open("wb") as dst:
            while True:
                block = src.read(64 * 1024)
                if not block:
                    break
                written = dst.write(block)
                if written != len(block):
                    raise FlashError(f"short UF2 write to {destination}: {written}/{len(block)}")
                bytes_written += written
            dst.flush()
            os.fsync(dst.fileno())
    except OSError as exc:
        if bytes_written == source_size:
            sync_filesystem()
            return destination
        raise FlashError(f"UF2 copy failed at {destination}: {exc}") from exc
    sync_filesystem()
    return destination


def flash_once(
    device_id: str,
    uf2: Path,
    timeout_s: float,
    *,
    no_1200_touch: bool = False,
    serial_by_id: Path = DEFAULT_SERIAL_BY_ID,
    disk_by_id: Path = DEFAULT_DISK_BY_ID,
) -> None:
    device_id = _validate_id(device_id)
    if not no_1200_touch:
        touch_1200(serial_port_for(device_id, serial_by_id))
    try:
        disk = wait_for_link(device_id, disk_by_id, timeout_s, disk=True)
    except FlashError:
        if not no_1200_touch:
            raise
        disk = uf2_disk_for(device_id, disk_by_id)
    root = mounted_uf2_root(disk, timeout_s)
    copy_uf2(uf2, root)
    wait_for_absence(device_id, disk_by_id, timeout_s)
    wait_for_link(device_id, serial_by_id, timeout_s, disk=False)


def list_devices(serial_by_id: Path = DEFAULT_SERIAL_BY_ID, disk_by_id: Path = DEFAULT_DISK_BY_ID) -> list[tuple[str, str]]:
    ids: set[str] = set()
    if serial_by_id.is_dir():
        for entry in serial_by_id.iterdir():
            for prefix in SERIAL_PREFIXES:
                if entry.name.startswith(prefix) and entry.name.endswith("-if00"):
                    ids.add(entry.name[len(prefix):-5].rstrip("-").upper())
    if disk_by_id.is_dir():
        for entry in disk_by_id.iterdir():
            if entry.name.startswith(DISK_PREFIX) and entry.name.endswith("-0:0"):
                ids.add(entry.name[len(DISK_PREFIX):-4].upper())
    result = []
    for device_id in sorted(ids):
        state = "application"
        try:
            uf2_disk_for(device_id, disk_by_id)
            state = "UF2"
        except FlashError:
            try:
                serial_port_for(device_id, serial_by_id)
            except FlashError:
                state = "unavailable"
        result.append((device_id, state))
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="list complete device IDs without writing")
    parser.add_argument("--device-id", help="complete USB serial number")
    parser.add_argument("--uf2", type=Path, help="final .uf2 firmware image")
    parser.add_argument("--role", choices=("master", "slave"), help="(deprecated) role label for progress messages")
    parser.add_argument("--wait-timeout", type=float, default=20.0)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--retry", type=int, default=0)
    parser.add_argument("--no-1200-touch", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.list:
        for device_id, state in list_devices():
            print(f"{device_id} {state}")
        return 0
    if not args.device_id or args.uf2 is None:
        parser.error("--device-id and --uf2 are required unless --list is used")
    if args.repeat < 1 or args.retry < 0 or args.wait_timeout <= 0:
        parser.error("--repeat must be >= 1, --retry >= 0, and --wait-timeout > 0")
    role_label = args.role or "unified"
    for cycle in range(1, args.repeat + 1):
        last_error: Exception | None = None
        for attempt in range(args.retry + 1):
            try:
                print(f"[{role_label}] {args.device_id} flash cycle {cycle}/{args.repeat} attempt {attempt + 1}", flush=True)
                flash_once(args.device_id, args.uf2, args.wait_timeout, no_1200_touch=args.no_1200_touch)
                last_error = None
                break
            except FlashError as exc:
                last_error = exc
                print(f"[{role_label}] {args.device_id} failed: {exc}", file=sys.stderr, flush=True)
        if last_error is not None:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
