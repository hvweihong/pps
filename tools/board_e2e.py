#!/usr/bin/env python3
"""Fixed-ID dual-board end-to-end validation gate."""

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import sys
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, TypeVar

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import flash_uf2

try:
    import serial
except ImportError:  # pragma: no cover - only needed for hardware operation
    serial = None


MASTER_ID = "DBE5C3D84EA2EC6F"
SLAVE_ID = "5B3D71D27A709CA2"
SLAVE2_ID = "DD01F9DF462D68A5"
SHELL_PROMPT = b"uart:~$ "
FLASH_TIMEOUT_S = 20.0
STATUS_TIMEOUT_S = 30.0
BRIDGE_TIMEOUT_S = 30.0
BRIDGE_MAX_LENGTH = 2048
BOOT_GUARD_CLEAR_MIN_UPTIME_MS = 10000
BOOT_GUARD_CLEAR_TIMEOUT_S = 30.0
HISTORY_MAX_BYTES = 256 * 1024
EVIDENCE_ROOT = Path("build") / "board-e2e"
MASTER_TO_SLAVE_SEED = 49
SLAVE_TO_MASTER_SEED = 114
UPLINK_ACK_LOSS_SEED = 213
THREE_BOARD_BRIDGE_LENGTHS = (1, 32, 128, 230, 600, 2048)
BEST_EFFORT_BROADCAST_ATTEMPTS = 3


class GateError(RuntimeError):
    """A fail-closed board gate error."""


class CommandTimeout(GateError):
    """A bounded shell command timed out with a captured receive tail."""

    def __init__(self, command_or_tail: str | bytes, tail: str | bytes | None = None):
        if tail is None:
            self.command = ""
            captured = command_or_tail
        else:
            self.command = str(command_or_tail)
            captured = tail
        self.tail = captured if isinstance(captured, bytes) else captured.encode()
        detail = f": {self.command}" if self.command else ""
        super().__init__(f"command timed out{detail}")


class RecoveryBudget:
    def __init__(self, limit: int = 1):
        self.limit = limit
        self.used = 0

    def claim(self, label: str) -> None:
        if self.used >= self.limit:
            raise GateError(f"{label}: automatic recovery budget exhausted")
        self.used += 1


@dataclass(frozen=True)
class BoardPlanItem:
    label: str
    role_id: int
    device_id: str
    uf2: Path


def resolve_serial(
    device_id: str,
    serial_by_id: Path = flash_uf2.DEFAULT_SERIAL_BY_ID,
) -> Path:
    if len(device_id) != 16 or any(
        character not in "0123456789abcdefABCDEF" for character in device_id
    ):
        raise GateError(f"invalid complete USB device ID: {device_id!r}")
    try:
        return flash_uf2.serial_port_for(device_id.upper(), serial_by_id)
    except flash_uf2.FlashError as exc:
        raise GateError(str(exc)) from exc


def _exact_application_identity_present(
    device_id: str,
    serial_by_id: Path = flash_uf2.DEFAULT_SERIAL_BY_ID,
) -> bool:
    if not serial_by_id.is_dir():
        raise GateError(f"serial identity directory is missing: {serial_by_id}")
    return any(
        entry.name.startswith(f"{prefix}{device_id.upper()}-if")
        for entry in serial_by_id.iterdir()
        for prefix in flash_uf2.SERIAL_PREFIXES
    )


def _wait_for_exact_flash_identity(device_id: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            resolve_serial(device_id)
            return False
        except GateError as serial_error:
            if _exact_application_identity_present(device_id):
                raise GateError(
                    f"application identity failed closed: {serial_error}"
                ) from serial_error
        try:
            flash_uf2.uf2_disk_for(device_id)
            return True
        except flash_uf2.FlashError as disk_error:
            if time.monotonic() >= deadline:
                raise GateError(
                    "timed out waiting for exact application or UF2 identity"
                ) from disk_error
        time.sleep(0.05)


CommandResult = TypeVar("CommandResult")


def _timeout_message(error: CommandTimeout, attempt: int, command: str) -> str:
    tail = error.tail.decode("utf-8", errors="backslashreplace")
    return (
        f"command timeout attempt {attempt}/2 command={command!r} receive_tail={tail!r}"
    )


def run_command_with_recovery(
    run_once: Callable[[str], CommandResult],
    recover: Callable[[], None],
    log: Callable[[str], None],
    command: str,
) -> CommandResult:
    try:
        return run_once(command)
    except CommandTimeout as exc:
        log(_timeout_message(exc, 1, command))
        recover()
    try:
        return run_once(command)
    except CommandTimeout as exc:
        log(_timeout_message(exc, 2, command))
        raise


def validate_stage_result(result: dict[str, object]) -> None:
    if result.get("master_to_slave") is not True:
        raise GateError("master-to-slave bridge direction did not pass")
    if result.get("slave_to_master") is not True:
        raise GateError("slave-to-master bridge direction did not pass")
    if result.get("queue_drops") != 0:
        raise GateError(f"queue drops must be zero: {result.get('queue_drops')}")
    recoveries = result.get("recoveries")
    recovery_limit = result.get("recovery_limit", 1)
    if (
        not isinstance(recoveries, int)
        or not isinstance(recovery_limit, int)
        or recoveries < 0
        or recovery_limit < 0
        or recoveries > recovery_limit
    ):
        raise GateError(
            f"recoveries must be within per-board budget: "
            f"recoveries={recoveries} limit={recovery_limit}"
        )


def _parse_role(output: str) -> int:
    match = re.search(
        r"(?m)^role_id = ([0-3]) \((?:persisted|default), reboot\)\s*$", output
    )
    if match is None:
        raise GateError("param get role_id returned unrecognized output")
    return int(match.group(1))


def _require_zero_drops(output: str, board: "str | BoardSession") -> None:
    board_label = board if isinstance(board, str) else board.label
    match = re.search(r"\binput_drop=(\d+)\s+output_drop=(\d+)\b", output)
    if match is None:
        raise GateError(f"{board_label}: bridge_test stats drop fields are missing")
    input_drop, output_drop = (int(value) for value in match.groups())
    if input_drop != 0 or output_drop != 0:
        if not isinstance(board, str):
            board.queue_drops += input_drop + output_drop
        raise GateError(
            f"{board_label}: bridge_test drops are non-zero: "
            f"input_drop={input_drop} output_drop={output_drop}"
        )


def _master_ready(output: str) -> bool:
    matches = re.findall(r"\bactive_count=(\d+)\b", output)
    return bool(matches) and int(matches[-1]) == 1


def _slave_ready(output: str) -> bool:
    matches = list(
        re.finditer(
            r"\bsync_state=(\d+)\b|\bState:\s+([A-Za-z]+)\b",
            output,
        )
    )
    if not matches:
        return False
    latest = matches[-1]
    return (
        latest.group(1) == "2"
        if latest.group(1) is not None
        else latest.group(2) == "LOCKED"
    )


def _current_pair_ready(master_output: str, slave_output: str) -> bool:
    return _master_ready(master_output) and _slave_ready(slave_output)


def _current_group_ready(
    master_output: str,
    slave_outputs: Iterable[str],
    *,
    expected_mask: int,
) -> bool:
    count_matches = re.findall(r"\bactive_count=(\d+)\b", master_output)
    mask_matches = re.findall(r"\bactive_mask=(0x[0-9A-Fa-f]+|\d+)\b", master_output)
    outputs = tuple(slave_outputs)

    return (
        bool(count_matches)
        and int(count_matches[-1]) == len(outputs)
        and bool(mask_matches)
        and int(mask_matches[-1], 0) == expected_mask
        and all(_slave_ready(output) for output in outputs)
    )


def _latency_summary_ms(samples: Iterable[float]) -> dict[str, float]:
    ordered = sorted(samples)
    if not ordered:
        raise GateError("latency samples must not be empty")
    p95_index = math.ceil(len(ordered) * 0.95) - 1
    return {
        "min": ordered[0],
        "median": statistics.median(ordered),
        "p95": ordered[p95_index],
        "max": ordered[-1],
    }


def _response_marker(command: str) -> bytes | None:
    if command == "param get role_id":
        return b"role_id = "
    if command.startswith("param set role_id "):
        return b"role_id set to "
    if command == "kernel uptime":
        return b"Uptime:"
    if command == "bridge_test clear":
        return b"bridge_test clear ok"
    if command == "time_sync status":
        return b"Time Synchronization Status:"
    if command.startswith("bridge_test inject "):
        return b"bridge_test inject"
    if command.startswith("bridge_test verify "):
        return b"bridge_test verify"
    if command.startswith("bridge_test verify_pair "):
        return b"bridge_test verify_pair"
    if command == "bridge_test stats":
        return b"bridge_test stats"
    return None


def _command_response_complete(received: bytes, command: str) -> bool:
    echo = command.encode("ascii")
    echo_at = received.find(echo)
    if echo_at < 0:
        return False
    marker = _response_marker(command)
    if marker is None:
        marker_at = echo_at + len(echo)
    else:
        marker_at = received.find(marker, echo_at + len(echo))
        if marker_at < 0:
            return False
        marker_at += len(marker)
    return received.find(SHELL_PROMPT, marker_at) >= 0


def _timestamp() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def _uf2_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as firmware:
            while chunk := firmware.read(1024 * 1024):
                digest.update(chunk)
    except OSError as exc:
        raise GateError(f"failed to hash UF2 {path}: {exc}") from exc
    return digest.hexdigest()


def _write_json_summary(path: Path, summary: dict[str, object]) -> None:
    with path.open("w", encoding="utf-8") as summary_file:
        json.dump(summary, summary_file, indent=2, sort_keys=True)
        summary_file.write("\n")
        summary_file.flush()
        os.fsync(summary_file.fileno())


def _exception_text(error: Exception) -> str:
    if isinstance(error, GateError):
        return str(error)
    return f"{type(error).__name__}: {error}"


class TimestampedRawLog:
    def __init__(self, path: Path):
        self.path = path
        self._file = path.open("ab", buffering=0)

    def record(self, direction: str, payload: bytes) -> None:
        prefix = f"[{_timestamp()}] {direction} ".encode("ascii")
        self._file.write(prefix + payload)
        if not payload.endswith(b"\n"):
            self._file.write(b"\n")
        os.fsync(self._file.fileno())

    def close(self) -> None:
        self._file.close()


class BoardSession:
    def __init__(
        self,
        label: str,
        device_id: str,
        uf2: Path,
        command_timeout: float,
        raw_log: TimestampedRawLog,
        recovery_budget: RecoveryBudget | None = None,
    ):
        self.label = label
        self.device_id = device_id.upper()
        self.uf2 = uf2
        self.command_timeout = command_timeout
        self.raw_log = raw_log
        self.recovery_budget = recovery_budget or RecoveryBudget()
        self.serial_port = None
        self.flash_count = 0
        self.flash_retry_count = 0
        self.recovery_count = 0
        self.queue_drops = 0
        self._history: deque[bytes] = deque()
        self._history_size = 0
        self._history_start = 0
        self._history_end = 0

    def event(self, message: str) -> None:
        print(f"[{self.label}] {message}", flush=True)
        self.raw_log.record("EVENT", message.encode("utf-8", errors="replace"))

    def _remember(self, payload: bytes) -> None:
        self._history.append(payload)
        self._history_size += len(payload)
        self._history_end += len(payload)
        while self._history_size > HISTORY_MAX_BYTES and self._history:
            discarded = self._history.popleft()
            self._history_size -= len(discarded)
            self._history_start += len(discarded)

    def clear_history(self) -> None:
        self._history.clear()
        self._history_size = 0
        self._history_start = self._history_end

    def history_text(self) -> str:
        return b"".join(self._history).decode("utf-8", errors="replace")

    def history_bytes(self) -> bytes:
        return b"".join(self._history)

    def history_mark(self) -> int:
        return self._history_end

    def history_since(self, mark: int) -> bytes:
        if mark >= self._history_end:
            return b""
        history = self.history_bytes()
        if mark <= self._history_start:
            return history
        return history[mark - self._history_start :]

    def close(self) -> None:
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            finally:
                self.serial_port = None

    def _flash_once_for_current_state(self) -> None:
        try:
            no_1200_touch = _wait_for_exact_flash_identity(
                self.device_id, max(FLASH_TIMEOUT_S, self.command_timeout)
            )
        except GateError as exc:
            raise GateError(f"{self.label}: {exc}") from exc
        if no_1200_touch:
            self.event("exact ID is in UF2 mode")
        else:
            try:
                self.connect()
            except GateError as exc:
                self.event(
                    "pre-flash shell unavailable; using exact-ID 1200-baud "
                    f"recovery: {exc}"
                )
            else:
                try:
                    _wait_for_boot_guard_clear(self)
                finally:
                    self.close()
        try:
            flash_uf2.flash_once(
                self.device_id,
                self.uf2,
                max(FLASH_TIMEOUT_S, self.command_timeout),
                no_1200_touch=no_1200_touch,
            )
        except flash_uf2.FlashError as exc:
            raise GateError(f"{self.label}: exact-ID flash failed: {exc}") from exc

    def flash(self) -> None:
        self.close()
        last_error: GateError | None = None
        for attempt in (1, 2):
            if attempt == 2:
                self.flash_retry_count += 1
            self.event(
                f"flash exact_id={self.device_id} attempt={attempt}/2 uf2={self.uf2}"
            )
            try:
                self._flash_once_for_current_state()
                self.flash_count += 1
                self.event(f"flash PASS exact_id={self.device_id} attempt={attempt}/2")
                return
            except GateError as exc:
                last_error = exc
                self.event(f"flash attempt={attempt}/2 failed: {exc}")
        raise last_error or GateError(f"{self.label}: exact-ID flash failed")

    def _read_available(self) -> bytes:
        if self.serial_port is None:
            return b""
        waiting = self.serial_port.in_waiting
        if waiting <= 0:
            return b""
        payload = self.serial_port.read(waiting)
        if payload:
            self.raw_log.record("RX", payload)
            self._remember(payload)
        return payload

    def _drain(self, duration: float = 0.15) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            if not self._read_available():
                time.sleep(0.01)

    def _write(self, payload: bytes) -> None:
        if self.serial_port is None:
            raise GateError(f"{self.label}: CDC is not connected")
        self.raw_log.record("TX", payload)
        try:
            written = self.serial_port.write(payload)
            self.serial_port.flush()
        except Exception as exc:
            raise CommandTimeout("<serial write>", b"") from exc
        if written != len(payload):
            raise CommandTimeout("<serial write>", b"")

    def _read_until_prompt(
        self, command: str, timeout: float, *, require_response: bool = True
    ) -> bytes:
        deadline = time.monotonic() + timeout
        received = bytearray()
        while time.monotonic() < deadline:
            try:
                payload = self._read_available()
            except Exception as exc:
                raise CommandTimeout(command, bytes(received[-4096:])) from exc
            if payload:
                received.extend(payload)
                if (
                    _command_response_complete(bytes(received), command)
                    if require_response
                    else SHELL_PROMPT in received
                ):
                    return bytes(received)
            else:
                time.sleep(0.01)
        raise CommandTimeout(command, bytes(received[-4096:]))

    def connect(self, wait_timeout: float | None = None) -> None:
        if serial is None:
            raise GateError("pyserial is required for CDC hardware validation")
        self.close()
        timeout = wait_timeout or max(FLASH_TIMEOUT_S, self.command_timeout * 2.0)
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                link = resolve_serial(self.device_id)
                self.serial_port = serial.Serial(
                    str(link),
                    baudrate=115200,
                    timeout=0,
                    write_timeout=1,
                    exclusive=True,
                )
                self._drain(0.2)
                self._write(b"\r")
                self._read_until_prompt(
                    "<shell readiness>", self.command_timeout, require_response=False
                )
                self.event(f"CDC shell ready exact_path={link}")
                return
            except (GateError, CommandTimeout, OSError, serial.SerialException) as exc:
                last_error = exc
                self.close()
                time.sleep(0.1)
        raise GateError(
            f"{self.label}: timed out waiting for application and shell readiness: {last_error}"
        ) from last_error

    def _command_once(self, command: str) -> str:
        if "\n" in command or "\r" in command:
            raise GateError(f"{self.label}: shell command must be exactly one line")
        self._drain()
        try:
            self._write(command.encode("ascii") + b"\r")
            output = self._read_until_prompt(command, self.command_timeout)
        except CommandTimeout as exc:
            if exc.command == "<serial write>":
                raise CommandTimeout(command, exc.tail) from exc
            raise
        return output.decode("utf-8", errors="replace")

    def recover(self) -> None:
        if self.recovery_count >= 1:
            raise GateError(f"{self.label}: automatic recovery budget exhausted")
        self.recovery_budget.claim(self.label)
        self.recovery_count += 1
        self.event(
            f"automatic command-timeout recovery={self.recovery_count} "
            f"reflash exact_id={self.device_id}"
        )
        self.close()
        try:
            self._flash_once_for_current_state()
        except GateError as exc:
            self.event(f"recovery flash attempt=1/1 failed: {exc}")
            raise
        self.flash_count += 1
        self.event(f"recovery flash PASS exact_id={self.device_id} attempt=1/1")
        self.connect()

    def command(self, command: str) -> str:
        return run_command_with_recovery(
            self._command_once,
            self.recover,
            self.event,
            command,
        )

    def reboot(self) -> None:
        self.reboot_and_disconnect()
        self.connect()

    def reboot_and_disconnect(self) -> None:
        self._drain()
        self.event("kernel reboot cold requested for role activation")
        self._write(b"kernel reboot cold\r")
        time.sleep(0.2)
        self.close()

    def watchdog_reset_begin(self) -> int:
        self._drain()
        mark = self.history_mark()
        self.event("watchdog reset requested")
        self._write(b"boot_test watchdog\r")
        time.sleep(0.2)
        self.close()
        return mark

    def require_watchdog_recovery(self, mark: int) -> None:
        for attempt in range(31):
            recovered = self.history_since(mark)
            if b"watchdog reset recovered" in recovered:
                self.event("watchdog reset retained diagnostic PASS")
                return
            if attempt < 30:
                self._drain(0.1)
        raise GateError(f"{self.label}: retained watchdog diagnostic is missing")

    def trigger_watchdog(self) -> None:
        mark = self.watchdog_reset_begin()
        self.connect(wait_timeout=max(40.0, self.command_timeout * 4.0))
        self.require_watchdog_recovery(mark)


def _expect(output: str, expected: str, board: str, command: str) -> None:
    if expected not in output:
        raise GateError(
            f"{board}: {command!r} did not produce expected text {expected!r}"
        )


def _ensure_role(board: BoardSession, required_role: int) -> bool:
    current_role = _parse_role(board.command("param get role_id"))
    changed = current_role != required_role
    if changed:
        output = board.command(f"param set role_id {required_role}")
        _expect(
            output,
            f"role_id set to {required_role}. Reboot required to apply.",
            board.label,
            "param set role_id",
        )
        board.reboot()
    applied_role = _parse_role(board.command("param get role_id"))
    if applied_role != required_role:
        raise GateError(
            f"{board.label}: role restore failed: expected={required_role} actual={applied_role}"
        )
    board.event(
        f"role_id={applied_role} state={'changed-and-rebooted' if changed else 'already-correct'}"
    )
    return changed


def _wait_for_radio_ready(
    master: BoardSession, slaves: BoardSession | Iterable[BoardSession]
) -> None:
    slave_list = list(slaves) if isinstance(slaves, (list, tuple)) else [slaves]
    expected_mask = (1 << len(slave_list)) - 1
    deadline = time.monotonic() + max(
        STATUS_TIMEOUT_S,
        master.command_timeout * 3.0,
        *(slave.command_timeout * 3.0 for slave in slave_list),
    )
    while time.monotonic() < deadline:
        master_output = master.command("bridge_test stats")
        slave_outputs = [slave.command("time_sync status") for slave in slave_list]
        if _current_group_ready(
            master_output, slave_outputs, expected_mask=expected_mask
        ):
            master.event(
                f"radio ready active_count={len(slave_list)} "
                f"active_mask=0x{expected_mask:02x}"
            )
            for slave in slave_list:
                slave.event("time sync ready sync_state=2/LOCKED")
            return
        time.sleep(0.25)
    raise GateError(
        "radio readiness timeout: current "
        f"active_count={len(slave_list)} active_mask=0x{expected_mask:02x} "
        "and all LOCKED samples missing"
    )


def _wait_for_verify(
    board: BoardSession,
    length: int,
    seed: int,
    direction: str,
) -> None:
    command = f"bridge_test verify {length} {seed}"
    expected = f"bridge_test verify ok len={length} seed={seed}"
    deadline = time.monotonic() + max(BRIDGE_TIMEOUT_S, board.command_timeout * 3.0)
    last_output = ""
    while time.monotonic() < deadline:
        last_output = board.command(command)
        if expected in last_output:
            board.event(
                f"bridge verify PASS direction={direction} len={length} seed={seed}"
            )
            return
        if "verify failed" in last_output or "verify overflow" in last_output:
            raise GateError(f"{board.label}: {direction} verify failed")
        if "verify pending" not in last_output:
            raise GateError(f"{board.label}: unrecognized bridge verify output")
        time.sleep(0.1)
    raise GateError(
        f"{board.label}: bridge verify timed out direction={direction}: "
        f"{last_output[-512:]}"
    )


def _wait_for_verify_pair(
    board: BoardSession,
    first_len: int,
    first_seed: int,
    second_len: int,
    second_seed: int,
) -> None:
    command = (
        f"bridge_test verify_pair {first_len} {first_seed} "
        f"{second_len} {second_seed}"
    )
    expected = (
        f"bridge_test verify_pair ok len1={first_len} seed1={first_seed} "
        f"len2={second_len} seed2={second_seed}"
    )
    deadline = time.monotonic() + max(BRIDGE_TIMEOUT_S, board.command_timeout * 3.0)
    last_output = ""
    while time.monotonic() < deadline:
        last_output = board.command(command)
        if expected in last_output:
            board.event(
                "bridge pair verify PASS "
                f"len1={first_len} seed1={first_seed} "
                f"len2={second_len} seed2={second_seed}"
            )
            return
        if "verify_pair failed" in last_output:
            raise GateError(f"{board.label}: whole-record pair verify failed")
        if "verify_pair pending" not in last_output:
            raise GateError(f"{board.label}: unrecognized bridge pair verify output")
        time.sleep(0.1)
    raise GateError(
        f"{board.label}: bridge pair verify timed out: {last_output[-512:]}"
    )


def _runtime_drop_values(output: str) -> dict[str, int] | None:
    values: dict[str, int] = {}
    for field in (
        "uart_rx_drop_bytes",
        "uart_tx_drop_bytes",
        "queue_drop_bytes",
        "radio_event_drop_count",
    ):
        matches = re.findall(rf"\b{field}=(\d+)\b", output)
        if not matches:
            return None
        values[field] = int(matches[-1])
    return values


def _require_runtime_zero_drops(output: str, board: str | BoardSession) -> None:
    board_label = board if isinstance(board, str) else board.label
    values = _runtime_drop_values(output)
    if values is None:
        raise GateError(f"{board_label}: one or more runtime drop fields are missing")
    observed_drops = sum(values.values())
    if not isinstance(board, str) and observed_drops:
        board.queue_drops = getattr(board, "queue_drops", 0) + observed_drops
    for field, value in values.items():
        if value != 0:
            raise GateError(f"{board_label}: {field}={value}, expected zero")


def _bridge_stat_value(output: str, field: str) -> int:
    matches = re.findall(rf"\b{re.escape(field)}=(\d+)\b", output)
    if not matches:
        raise GateError(f"bridge stat field is missing: {field}")
    return int(matches[-1])


MASTER_RECORD_DIAGNOSTIC_FIELDS = (
    "node1_records",
    "node1_bytes",
    "node1_record_drop",
    "uart_record_queued",
    "uart_record_completed",
    "uart_record_aborted",
    "uart_record_rejected",
    "uart_record_pending",
    "uart_record_busy",
    "uart_start_errors",
)


def _master_record_diagnostic_values(output: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for field in MASTER_RECORD_DIAGNOSTIC_FIELDS:
        matches = re.findall(rf"\b{field}=(\d+)\b", output)
        if not matches:
            raise GateError(f"master: record diagnostic field is missing: {field}")
        values[field] = int(matches[-1])
    return values


def _node_record_values(output: str, node_id: int) -> dict[str, int]:
    if node_id not in (1, 2, 3):
        raise GateError(f"master: invalid fixed node ID {node_id}")
    return {
        "records": _bridge_stat_value(output, f"node{node_id}_records"),
        "bytes": _bridge_stat_value(output, f"node{node_id}_bytes"),
        "drops": _bridge_stat_value(output, f"node{node_id}_record_drop"),
    }


def _wait_for_node_record_deltas(
    master: BoardSession,
    baselines: dict[int, dict[str, int]],
    expected_bytes: dict[int, int],
) -> None:
    deadline = time.monotonic() + max(STATUS_TIMEOUT_S, master.command_timeout * 3.0)
    last_output = ""
    while time.monotonic() < deadline:
        last_output = master.command("bridge_test stats")
        current = {
            node_id: _node_record_values(last_output, node_id)
            for node_id in baselines
        }
        for node_id, baseline in baselines.items():
            expected = baseline["bytes"] + expected_bytes.get(node_id, 0)
            if current[node_id]["bytes"] > expected:
                raise GateError(
                    f"master: node{node_id}_bytes={current[node_id]['bytes']} "
                    f"exceeded expected {expected}"
                )
            if current[node_id]["drops"] != baseline["drops"]:
                raise GateError(f"master: node{node_id} record drop increased")
        uart_pending = _bridge_stat_value(last_output, "uart_record_pending")
        uart_busy = _bridge_stat_value(last_output, "uart_record_busy")
        uart_queued = _bridge_stat_value(last_output, "uart_record_queued")
        uart_completed = _bridge_stat_value(last_output, "uart_record_completed")
        if (
            all(
                current[node_id]["bytes"]
                == baseline["bytes"] + expected_bytes.get(node_id, 0)
                and (
                    expected_bytes.get(node_id, 0) == 0
                    or current[node_id]["records"] > baseline["records"]
                )
                for node_id, baseline in baselines.items()
            )
            and uart_pending == 0
            and uart_busy == 0
            and uart_queued == uart_completed
        ):
            master.event(
                "fixed-node record attribution PASS "
                + " ".join(
                    f"node{node_id}_bytes={current[node_id]['bytes']}"
                    for node_id in sorted(current)
                )
            )
            return
        time.sleep(0.05)
    raise GateError(
        "master: timed out waiting for fixed-node record attribution: "
        f"{last_output[-512:]}"
    )


def _clear_validation(board: BoardSession) -> None:
    output = board.command("bridge_test clear")
    _expect(output, "bridge_test clear ok", board.label, "bridge_test clear")


def _inject_pattern(
    board: BoardSession, length: int, seed: int, *, paced: bool = False
) -> None:
    command_name = "inject_uart" if paced else "inject"
    command = f"bridge_test {command_name} {length} {seed}"
    output = board.command(command)
    _expect(
        output,
        f"bridge_test {command_name} ok len={length} seed={seed}",
        board.label,
        command,
    )


def _elapsed_ms(start: float) -> float:
    return round((time.monotonic() - start) * 1000.0, 3)


def _run_best_effort_broadcast_gate(
    master: BoardSession,
    slaves: list[BoardSession],
    length: int,
    seed: int,
    direction: str,
) -> dict[str, float]:
    if not slaves:
        raise GateError("best-effort broadcast gate requires at least one slave")
    for attempt in range(1, BEST_EFFORT_BROADCAST_ATTEMPTS + 1):
        attempt_seed = (seed + attempt - 1) & 0xFF
        for board in (master, *slaves):
            _clear_validation(board)
        started = time.monotonic()
        _inject_pattern(master, length, attempt_seed, paced=True)
        latency: dict[str, float] = {}
        missing: list[str] = []
        for slave in slaves:
            command = f"bridge_test verify {length} {attempt_seed}"
            expected = f"bridge_test verify ok len={length} seed={attempt_seed}"
            output = slave.command(command)
            if expected in output:
                latency[slave.label] = _elapsed_ms(started)
                continue
            if "verify failed" in output or "verify overflow" in output:
                raise GateError(f"{slave.label}: {direction} verify failed")
            if "verify pending" not in output:
                raise GateError(
                    f"{slave.label}: unrecognized best-effort bridge verify output"
                )
            missing.append(slave.label)
        if not missing:
            for slave in slaves:
                slave.event(
                    f"bridge verify PASS direction={direction}-{slave.label} "
                    f"len={length} seed={attempt_seed}"
                )
            master.event(
                f"best-effort broadcast gate PASS direction={direction} "
                f"attempt={attempt}/{BEST_EFFORT_BROADCAST_ATTEMPTS}"
            )
            return latency
        master.event(
            f"best-effort broadcast retry direction={direction} "
            f"attempt={attempt}/{BEST_EFFORT_BROADCAST_ATTEMPTS} "
            f"missing={','.join(missing)}"
        )
    raise GateError(
        f"{direction}: best-effort broadcast missed after "
        f"{BEST_EFFORT_BROADCAST_ATTEMPTS} independent attempts"
    )


def _merge_latency_samples(
    target: dict[str, list[float]], source: dict[str, list[float]] | None
) -> None:
    if source is None:
        return
    for direction, samples in source.items():
        target.setdefault(direction, []).extend(samples)


def _require_master_record_diagnostics(
    output: str,
    *,
    expected_bytes: int,
    board: str,
    minimum_records: int = 1,
) -> dict[str, int]:
    values = _master_record_diagnostic_values(output)
    if values["node1_records"] < minimum_records:
        raise GateError(
            f"{board}: node1_records={values['node1_records']}, "
            f"expected at least {minimum_records}"
        )
    if values["node1_bytes"] != expected_bytes:
        raise GateError(
            f"{board}: node1_bytes={values['node1_bytes']}, expected {expected_bytes}"
        )
    for field in (
        "node1_record_drop",
        "uart_record_rejected",
        "uart_record_pending",
        "uart_record_busy",
        "uart_start_errors",
    ):
        if values[field] != 0:
            raise GateError(f"{board}: {field}={values[field]}, expected zero")
    if values["uart_record_queued"] != values["uart_record_completed"]:
        raise GateError(
            f"{board}: UART record queue is not idle: "
            f"queued={values['uart_record_queued']} "
            f"completed={values['uart_record_completed']}"
        )
    if values["uart_record_queued"] != values["node1_records"]:
        raise GateError(
            f"{board}: UART/node record attribution differs: "
            f"queued={values['uart_record_queued']} "
            f"node1={values['node1_records']}"
        )
    return values


def _wait_for_master_record_diagnostics(
    master: BoardSession,
    *,
    expected_bytes: int,
    minimum_records: int,
) -> dict[str, int]:
    deadline = time.monotonic() + max(STATUS_TIMEOUT_S, master.command_timeout * 3.0)
    last_output = ""
    while time.monotonic() < deadline:
        last_output = master.command("bridge_test stats")
        values = _master_record_diagnostic_values(last_output)
        for field in (
            "node1_record_drop",
            "uart_record_rejected",
            "uart_start_errors",
        ):
            if values[field] != 0:
                raise GateError(f"master: {field}={values[field]}, expected zero")
        if values["node1_bytes"] > expected_bytes:
            raise GateError(
                f"master: node1_bytes={values['node1_bytes']} exceeded "
                f"expected {expected_bytes}"
            )
        if (
            values["node1_bytes"] == expected_bytes
            and values["node1_records"] >= minimum_records
            and values["uart_record_pending"] == 0
            and values["uart_record_busy"] == 0
            and values["uart_record_queued"] == values["uart_record_completed"]
        ):
            checked = _require_master_record_diagnostics(
                last_output,
                expected_bytes=expected_bytes,
                board=master.label,
                minimum_records=minimum_records,
            )
            master.event(
                "master record diagnostics PASS "
                f"node1_records={checked['node1_records']} "
                f"node1_bytes={checked['node1_bytes']} "
                f"uart_aborted={checked['uart_record_aborted']}"
            )
            return checked
        time.sleep(0.05)
    raise GateError(
        "master: timed out waiting for record/UART idle diagnostics: "
        f"{last_output[-512:]}"
    )


def _wait_for_fresh_runtime_zero_drops(board: BoardSession) -> None:
    output = board.command("bridge_test stats")
    _require_runtime_zero_drops(output, board)
    board.event(
        "runtime queue drops PASS uart_rx=0 uart_tx=0 "
        "bridge_queue=0 radio_event=0"
    )


def _wait_for_boot_guard_clear(board: BoardSession) -> None:
    """Do not issue another cold reset while the persisted boot guard is armed."""
    run_command = getattr(board, "_command_once", board.command)
    deadline = time.monotonic() + max(
        BOOT_GUARD_CLEAR_TIMEOUT_S, board.command_timeout * 3.0
    )
    while time.monotonic() < deadline:
        output = run_command("kernel uptime")
        match = re.search(r"\bUptime:\s*(\d+)\s*ms\b", output)
        if match is None:
            raise GateError(f"{board.label}: kernel uptime was not parseable")
        uptime_ms = int(match.group(1))
        if uptime_ms >= BOOT_GUARD_CLEAR_MIN_UPTIME_MS:
            board.event(
                "boot guard clear PASS "
                f"uptime_ms={uptime_ms} minimum_ms={BOOT_GUARD_CLEAR_MIN_UPTIME_MS}"
            )
            return
        time.sleep(0.1)
    raise GateError(f"{board.label}: timed out waiting for boot guard clear uptime")


TIME_UART_ZERO_FIELDS = (
    "rx_drop_bytes",
    "overlong_line_drops",
    "output_line_drops",
    "rx_restart_errors",
    "rx_buffer_errors",
    "rx_stopped_events",
    "rx_stop_reason_mask",
    "pps_input_drop_count",
)


def _time_uart_error_values(output: str) -> dict[str, int] | None:
    status_lines = re.findall(r"\btime_uart\b[^\r\n]*", output)
    if not status_lines:
        return None
    status_line = status_lines[-1]
    values = {}
    for field in TIME_UART_ZERO_FIELDS:
        match = re.search(rf"\b{field}=(\d+)\b", status_line)
        if match is None:
            return None
        values[field] = int(match.group(1))
    return values


def _wait_for_fresh_time_uart_zero_errors(board: BoardSession) -> None:
    output = board.command("bridge_test stats")
    values = _time_uart_error_values(output)
    if values is None:
        raise GateError(f"{board.label}: on-demand time UART status is incomplete")
    nonzero = {field: value for field, value in values.items() if value != 0}
    if nonzero:
        details = " ".join(f"{field}={value}" for field, value in nonzero.items())
        raise GateError(f"{board.label}: time UART input errors or drops {details}")
    board.event("time UART input errors and drops PASS")


def _uptime_ms(board: BoardSession) -> int:
    output = board.command("kernel uptime")
    match = re.search(r"\bUptime:\s*(\d+)\s*ms\b", output)
    if match is None:
        raise GateError(f"{board.label}: kernel uptime was not parseable")
    return int(match.group(1))


def _wait_for_active_mask(master: BoardSession, expected_mask: int) -> None:
    deadline = time.monotonic() + max(STATUS_TIMEOUT_S, master.command_timeout * 3.0)
    expected_count = expected_mask.bit_count()
    last_output = ""
    while time.monotonic() < deadline:
        last_output = master.command("bridge_test stats")
        count_matches = re.findall(r"\bactive_count=(\d+)\b", last_output)
        mask_matches = re.findall(
            r"\bactive_mask=(0x[0-9A-Fa-f]+|\d+)\b", last_output
        )
        if (
            count_matches
            and int(count_matches[-1]) == expected_count
            and mask_matches
            and int(mask_matches[-1], 0) == expected_mask
        ):
            master.event(
                f"active mask transition PASS active_count={expected_count} "
                f"active_mask=0x{expected_mask:02x}"
            )
            return
        time.sleep(0.05)
    raise GateError(
        f"master: active mask 0x{expected_mask:02x} transition timed out: "
        f"{last_output[-512:]}"
    )


def _run_three_board_smoke_gate(
    master: BoardSession, slaves: list[BoardSession], bridge_length: int
) -> None:
    if len(slaves) != 2:
        raise GateError("three-board smoke gate requires exactly two slaves")
    smoke_length = min(max(bridge_length, 1), 128)
    _run_best_effort_broadcast_gate(
        master,
        slaves,
        smoke_length,
        41,
        "restart-smoke-master-broadcast",
    )
    for node_id, slave, seed in ((1, slaves[0], 73), (2, slaves[1], 109)):
        _clear_validation(master)
        baseline_output = master.command("bridge_test stats")
        baselines = {
            fixed_node: _node_record_values(baseline_output, fixed_node)
            for fixed_node in (1, 2)
        }
        _inject_pattern(slave, smoke_length, seed, paced=True)
        _wait_for_verify(
            master,
            smoke_length,
            seed,
            f"restart-smoke-{slave.label}-to-master",
        )
        _wait_for_node_record_deltas(
            master, baselines, {node_id: smoke_length}
        )


def _run_bidirectional_bridge_gate(
    master: BoardSession, slave: BoardSession, bridge_length: int, label: str
) -> dict[str, list[float]]:
    master.clear_history()
    slave.clear_history()

    baseline_output = master.command("bridge_test stats")
    baseline = _master_record_diagnostic_values(baseline_output)
    _require_master_record_diagnostics(
        baseline_output,
        expected_bytes=baseline["node1_bytes"],
        board=master.label,
        minimum_records=baseline["node1_records"],
    )

    master_to_slave_started = time.monotonic()
    _inject_pattern(master, bridge_length, MASTER_TO_SLAVE_SEED)
    _wait_for_verify(
        slave, bridge_length, MASTER_TO_SLAVE_SEED, f"{label}-master-to-slave"
    )
    master_to_slave_ms = _elapsed_ms(master_to_slave_started)

    slave_to_master_started = time.monotonic()
    _inject_pattern(slave, bridge_length, SLAVE_TO_MASTER_SEED)
    _wait_for_verify(
        master, bridge_length, SLAVE_TO_MASTER_SEED, f"{label}-slave-to-master"
    )
    slave_to_master_ms = _elapsed_ms(slave_to_master_started)
    _wait_for_master_record_diagnostics(
        master,
        expected_bytes=baseline["node1_bytes"] + bridge_length,
        minimum_records=baseline["node1_records"] + 1,
    )

    for board in (master, slave):
        stats = board.command("bridge_test stats")
        _require_zero_drops(stats, board)
        board.event("bridge_test stats PASS input_drop=0 output_drop=0")
        _wait_for_fresh_runtime_zero_drops(board)
        _wait_for_fresh_time_uart_zero_errors(board)
    return {
        "master_to_slave": [master_to_slave_ms],
        "slave_to_master": [slave_to_master_ms],
    }


def _run_three_board_bridge_gate(
    master: BoardSession, slaves: list[BoardSession]
) -> dict[str, list[float]]:
    if len(slaves) != 2:
        raise GateError("three-board bridge gate requires exactly two slaves")
    slave1, slave2 = slaves
    latency: dict[str, list[float]] = {
        "master_to_slave1": [],
        "master_to_slave2": [],
        "slave1_to_master": [],
        "slave2_to_master": [],
        "pair_to_master": [],
    }

    for index, length in enumerate(THREE_BOARD_BRIDGE_LENGTHS):
        downlink_seed = 16 + index * 7
        downlink_latency = _run_best_effort_broadcast_gate(
            master,
            slaves,
            length,
            downlink_seed,
            f"matrix-master-broadcast-{length}",
        )
        latency["master_to_slave1"].append(downlink_latency[slave1.label])
        latency["master_to_slave2"].append(downlink_latency[slave2.label])

        for node_id, slave, seed_base in (
            (1, slave1, 96),
            (2, slave2, 160),
        ):
            _clear_validation(master)
            baseline_output = master.command("bridge_test stats")
            baselines = {
                fixed_node: _node_record_values(baseline_output, fixed_node)
                for fixed_node in (1, 2)
            }
            uplink_seed = seed_base + index * 7
            started = time.monotonic()
            _inject_pattern(slave, length, uplink_seed, paced=True)
            _wait_for_verify(
                master,
                length,
                uplink_seed,
                f"matrix-{slave.label}-to-master-{length}",
            )
            latency[f"{slave.label}_to_master"].append(_elapsed_ms(started))
            _wait_for_node_record_deltas(
                master, baselines, {node_id: length}
            )

    pair_lengths = (32, 48)
    pair_seeds = (17, 91)
    _clear_validation(master)
    baseline_output = master.command("bridge_test stats")
    baselines = {
        node_id: _node_record_values(baseline_output, node_id)
        for node_id in (1, 2)
    }
    started = time.monotonic()
    _inject_pattern(slave1, pair_lengths[0], pair_seeds[0], paced=True)
    _inject_pattern(slave2, pair_lengths[1], pair_seeds[1], paced=True)
    _wait_for_verify_pair(
        master,
        pair_lengths[0],
        pair_seeds[0],
        pair_lengths[1],
        pair_seeds[1],
    )
    latency["pair_to_master"].append(_elapsed_ms(started))
    _wait_for_node_record_deltas(
        master,
        baselines,
        {1: pair_lengths[0], 2: pair_lengths[1]},
    )

    for board in (master, slave1, slave2):
        stats = board.command("bridge_test stats")
        _require_zero_drops(stats, board)
        _wait_for_fresh_runtime_zero_drops(board)
        _wait_for_fresh_time_uart_zero_errors(board)
    return latency


def _run_downlink_loss_gate(
    master: BoardSession,
    slave: BoardSession,
    bridge_length: int,
    every_n: int,
) -> None:
    stats = slave.command("bridge_test stats")
    initial_gap = _bridge_stat_value(stats, "downlink_gap")
    if initial_gap != 0:
        raise GateError(f"slave: downlink_gap={initial_gap}, expected zero before loss")

    command = f"bridge_test loss 4 {every_n}"
    output = master.command(command)
    _expect(
        output,
        f"bridge_test loss ok mask=0x00000010 every_n={every_n}",
        master.label,
        command,
    )
    try:
        for seed in (49, 98):
            command = f"bridge_test inject {bridge_length} {seed}"
            output = master.command(command)
            _expect(
                output,
                f"bridge_test inject ok len={bridge_length} seed={seed}",
                master.label,
                command,
            )

        deadline = time.monotonic() + max(BRIDGE_TIMEOUT_S, slave.command_timeout * 3.0)
        last_stats = ""
        while time.monotonic() < deadline:
            last_stats = slave.command("bridge_test stats")
            if _bridge_stat_value(last_stats, "downlink_gap") > initial_gap:
                break
            time.sleep(0.1)
        else:
            raise GateError(
                "slave: downlink gap counter did not increase under injected loss: "
                f"{last_stats[-512:]}"
            )

        command = f"bridge_test inject {bridge_length} 147"
        output = slave.command(command)
        _expect(
            output,
            f"bridge_test inject ok len={bridge_length} seed=147",
            slave.label,
            command,
        )
        _wait_for_verify(master, bridge_length, 147, "loss-active-slave-to-master")
    finally:
        output = master.command("bridge_test loss_off")
        _expect(
            output,
            "bridge_test loss_off ok mask=0x00000000 every_n=0",
            master.label,
            "bridge_test loss_off",
        )

    for board in (master, slave):
        output = board.command("bridge_test clear")
        _expect(output, "bridge_test clear ok", board.label, "bridge_test clear")

    command = f"bridge_test inject {bridge_length} 196"
    output = master.command(command)
    _expect(
        output,
        f"bridge_test inject ok len={bridge_length} seed=196",
        master.label,
        command,
    )
    _wait_for_verify(slave, bridge_length, 196, "loss-off-master-to-slave")
    final_stats = slave.command("bridge_test stats")
    final_gap = _bridge_stat_value(final_stats, "downlink_gap")
    if final_gap <= initial_gap:
        raise GateError(
            f"slave: downlink_gap={final_gap}, expected greater than {initial_gap}"
        )
    for board in (master, slave):
        stats = board.command("bridge_test stats")
        _require_zero_drops(stats, board)
        _wait_for_fresh_runtime_zero_drops(board)
        _wait_for_fresh_time_uart_zero_errors(board)
    master.event(
        f"downlink loss gate PASS frame_type=4 every_n={every_n} "
        f"slave_gap_before={initial_gap} slave_gap_after={final_gap}"
    )
    slave.event(
        f"downlink loss gate PASS frame_type=4 every_n={every_n} "
        f"slave_gap_before={initial_gap} slave_gap_after={final_gap}"
    )


def _run_uplink_ack_loss_once_gate(
    master: BoardSession,
    slave: BoardSession,
    bridge_length: int,
) -> None:
    for board in (master, slave):
        output = board.command("bridge_test clear")
        _expect(output, "bridge_test clear ok", board.label, "bridge_test clear")

    baseline_output = master.command("bridge_test stats")
    baseline = _master_record_diagnostic_values(baseline_output)
    _require_master_record_diagnostics(
        baseline_output,
        expected_bytes=baseline["node1_bytes"],
        board=master.label,
        minimum_records=baseline["node1_records"],
    )

    command = "bridge_test loss_once 7"
    output = slave.command(command)
    _expect(
        output,
        "bridge_test loss_once ok mask=0x00000080 min_len=23",
        slave.label,
        command,
    )
    try:
        command = f"bridge_test inject {bridge_length} {UPLINK_ACK_LOSS_SEED}"
        output = slave.command(command)
        _expect(
            output,
            f"bridge_test inject ok len={bridge_length} seed={UPLINK_ACK_LOSS_SEED}",
            slave.label,
            command,
        )
        _wait_for_verify(
            master,
            bridge_length,
            UPLINK_ACK_LOSS_SEED,
            "single-ack-loss-slave-to-master",
        )
        slave_stats = slave.command("bridge_test stats")
        if _bridge_stat_value(slave_stats, "loss_dropped") != 1:
            raise GateError("slave: one-shot ACK_UPLINK loss was not observed")
        master_diag = _wait_for_master_record_diagnostics(
            master,
            expected_bytes=baseline["node1_bytes"] + bridge_length,
            minimum_records=baseline["node1_records"] + 1,
        )
        master_stats = master.command("bridge_test stats")
        if _bridge_stat_value(master_stats, "output") != 0:
            raise GateError("master: duplicate payload remained after exact verify")
    finally:
        output = slave.command("bridge_test loss_off")
        _expect(
            output,
            "bridge_test loss_off ok mask=0x00000000 every_n=0",
            slave.label,
            "bridge_test loss_off",
        )

    for board in (master, slave):
        stats = board.command("bridge_test stats")
        _require_zero_drops(stats, board)
        _wait_for_fresh_runtime_zero_drops(board)
    master.event(
        "uplink ACK one-shot loss gate PASS "
        f"frame_type=7 dropped=1 node1_bytes={master_diag['node1_bytes']}"
    )
    slave.event("uplink ACK one-shot loss gate PASS frame_type=7 dropped=1")


def _run_cold_reboot_cycles(
    master: BoardSession,
    slaves: BoardSession | Iterable[BoardSession],
    cycles: int,
    bridge_length: int,
) -> list[dict[str, object]]:
    slave_list = list(slaves) if isinstance(slaves, (list, tuple)) else [slaves]
    boards = [master, *slave_list]
    full_mask = (1 << len(slave_list)) - 1
    records: list[dict[str, object]] = []
    for board in boards:
        for cycle in range(1, cycles + 1):
            before_uptime = _uptime_ms(board)
            started = time.monotonic()
            board.event(
                f"cold reboot board={board.label} cycle={cycle}/{cycles} "
                f"pre_uptime_ms={before_uptime}"
            )
            board.reboot_and_disconnect()
            if board in slave_list:
                node_index = slave_list.index(board)
                _wait_for_active_mask(master, full_mask & ~(1 << node_index))
            board.connect()
            time.sleep(1.0)
            after_uptime = _uptime_ms(board)
            if after_uptime >= before_uptime:
                raise GateError(
                    f"{board.label}: cold reboot did not reduce uptime "
                    f"before={before_uptime} after={after_uptime}"
                )
            _wait_for_boot_guard_clear(board)
            _wait_for_radio_ready(
                master, slave_list[0] if len(slave_list) == 1 else slave_list
            )
            for checked_board in boards:
                _wait_for_fresh_runtime_zero_drops(checked_board)
                _wait_for_fresh_time_uart_zero_errors(checked_board)
            if len(slave_list) == 2:
                _run_three_board_smoke_gate(master, slave_list, bridge_length)
            else:
                _run_bidirectional_bridge_gate(
                    master,
                    slave_list[0],
                    min(bridge_length, 128),
                    f"post-cold-reboot-{board.label}-{cycle}",
                )
            duration_ms = _elapsed_ms(started)
            records.append(
                {
                    "kind": "cold",
                    "board": board.label,
                    "cycle": cycle,
                    "duration_ms": duration_ms,
                    "post_uptime_ms": after_uptime,
                }
            )
            board.event(
                f"cold reboot board={board.label} cycle={cycle}/{cycles} PASS "
                f"post_uptime_ms={after_uptime} peer_ready=PASS "
                f"duration_ms={duration_ms}"
            )
    return records


def _run_watchdog_reboot_cycles(
    master: BoardSession,
    slaves: BoardSession | Iterable[BoardSession],
    cycles: int,
    bridge_length: int,
) -> list[dict[str, object]]:
    slave_list = list(slaves) if isinstance(slaves, (list, tuple)) else [slaves]
    boards = [master, *slave_list]
    full_mask = (1 << len(slave_list)) - 1
    records: list[dict[str, object]] = []
    for board in boards:
        for cycle in range(1, cycles + 1):
            started = time.monotonic()
            board.event(
                f"watchdog reboot board={board.label} cycle={cycle}/{cycles} start"
            )
            history_mark = board.watchdog_reset_begin()
            if board in slave_list:
                node_index = slave_list.index(board)
                _wait_for_active_mask(master, full_mask & ~(1 << node_index))
            board.connect(wait_timeout=max(40.0, board.command_timeout * 4.0))
            board.require_watchdog_recovery(history_mark)
            _wait_for_boot_guard_clear(board)
            _wait_for_radio_ready(
                master, slave_list[0] if len(slave_list) == 1 else slave_list
            )
            for checked_board in boards:
                _wait_for_fresh_runtime_zero_drops(checked_board)
                _wait_for_fresh_time_uart_zero_errors(checked_board)
            if len(slave_list) == 2:
                _run_three_board_smoke_gate(master, slave_list, bridge_length)
            else:
                _run_bidirectional_bridge_gate(
                    master,
                    slave_list[0],
                    min(bridge_length, 128),
                    f"post-watchdog-{board.label}-{cycle}",
                )
            duration_ms = _elapsed_ms(started)
            records.append(
                {
                    "kind": "watchdog",
                    "board": board.label,
                    "cycle": cycle,
                    "duration_ms": duration_ms,
                }
            )
            board.event(
                f"watchdog reboot board={board.label} cycle={cycle}/{cycles} PASS "
                f"duration_ms={duration_ms}"
            )
    return records


def _run_steady_state_window(
    master: BoardSession,
    slaves: BoardSession | Iterable[BoardSession],
    seconds: float,
) -> None:
    slave_list = list(slaves) if isinstance(slaves, (list, tuple)) else [slaves]
    boards = [master, *slave_list]
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        _wait_for_radio_ready(
            master, slave_list[0] if len(slave_list) == 1 else slave_list
        )
        for board in boards:
            _wait_for_fresh_runtime_zero_drops(board)
            _wait_for_fresh_time_uart_zero_errors(board)
        time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
    for board in boards:
        board.event(f"steady state PASS seconds={seconds:g}")


def _run_cycle(
    master: BoardSession,
    slaves: BoardSession | Iterable[BoardSession],
    cycle: int,
    cycles: int,
    bridge_length: int,
    bridge_repeats: int,
) -> dict[str, list[float]]:
    slave_list = list(slaves) if isinstance(slaves, (list, tuple)) else [slaves]
    boards = [master, *slave_list]
    latency: dict[str, list[float]] = {}

    for board in boards:
        board.event(f"cycle={cycle}/{cycles} start")
        board.flash()
    for board in boards:
        board.connect()
    _ensure_role(master, 0)
    for role_id, slave in enumerate(slave_list, start=1):
        _ensure_role(slave, role_id)
    for board in boards:
        board.clear_history()
        uptime = board.command("kernel uptime")
        _expect(uptime, "Uptime:", board.label, "kernel uptime")
        _clear_validation(board)

    _wait_for_radio_ready(master, slave_list[0] if len(slave_list) == 1 else slave_list)

    if len(slave_list) == 2:
        _merge_latency_samples(
            latency, _run_three_board_bridge_gate(master, slave_list)
        )
    elif len(slave_list) == 1:
        for repeat in range(1, bridge_repeats + 1):
            _merge_latency_samples(
                latency,
                _run_bidirectional_bridge_gate(
                    master,
                    slave_list[0],
                    bridge_length,
                    f"cycle-{cycle}-repeat-{repeat}",
                ),
            )
    else:
        raise GateError("board cycle requires one or two slaves")
    for board in boards:
        board.event(f"cycle={cycle}/{cycles} PASS")
    return latency


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stage", required=True, help="stage label used in evidence paths"
    )
    parser.add_argument("--master-id", default=MASTER_ID, help="complete master USB ID")
    parser.add_argument("--slave-id", default=SLAVE_ID, help="complete slave USB ID")
    parser.add_argument("--slave2-id", help="complete second-slave USB ID")
    parser.add_argument("--master-uf2", required=True, type=Path)
    parser.add_argument("--slave-uf2", required=True, type=Path)
    parser.add_argument("--slave2-uf2", type=Path)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument(
        "--cold-reboots-per-board",
        type=int,
        default=0,
        help="Sequential cold reboots per board after flashing; waits >=10s guard clear",
    )
    parser.add_argument(
        "--watchdog-restarts-per-board",
        type=int,
        default=0,
        help="Sequential retained-diagnostic watchdog restarts per board",
    )
    parser.add_argument(
        "--steady-state-seconds",
        type=float,
        default=0.0,
        help="Post-gate radio and UART health observation window",
    )
    parser.add_argument("--bridge-length", type=int, default=600)
    parser.add_argument(
        "--bridge-repeats",
        type=int,
        default=1,
        help="Bidirectional bridge repetitions per flash cycle",
    )
    parser.add_argument(
        "--downlink-loss-every-n",
        type=int,
        default=0,
        help="Run best-effort downlink loss gate; 0 disables, otherwise use N >= 2",
    )
    parser.add_argument(
        "--uplink-ack-loss-once",
        action="store_true",
        help="Drop one non-empty ACK_UPLINK and verify exact retry delivery",
    )
    parser.add_argument("--command-timeout", type=float, default=10.0)
    return parser


def _validated_id(device_id: str, option: str) -> str:
    if len(device_id) != 16 or re.fullmatch(r"[0-9A-Fa-f]{16}", device_id) is None:
        raise GateError(f"{option} must be a complete 16-hex-digit USB ID")
    return device_id.upper()


def _validate_board_plan(args: argparse.Namespace) -> tuple[BoardPlanItem, ...]:
    second_id = args.slave2_id
    second_uf2 = args.slave2_uf2
    if (second_id is None) != (second_uf2 is None):
        raise GateError("--slave2-id and --slave2-uf2 must be provided together")

    master_id = _validated_id(args.master_id, "--master-id")
    slave_id = _validated_id(args.slave_id, "--slave-id")
    if second_id is None:
        if master_id == slave_id:
            raise GateError("master and slave USB IDs must be distinct")
        return (
            BoardPlanItem("master", 0, master_id, args.master_uf2),
            BoardPlanItem("slave", 1, slave_id, args.slave_uf2),
        )

    slave2_id = _validated_id(second_id, "--slave2-id")
    if len({master_id, slave_id, slave2_id}) != 3:
        raise GateError("master, slave1, and slave2 USB IDs must be distinct")
    return (
        BoardPlanItem("master", 0, master_id, args.master_uf2),
        BoardPlanItem("slave1", 1, slave_id, args.slave_uf2),
        BoardPlanItem("slave2", 2, slave2_id, second_uf2),
    )


def main(argv: Iterable[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    started_at = _timestamp()
    safe_stage = re.sub(r"[^A-Za-z0-9_.-]+", "-", args.stage).strip("-.")
    run_stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence_dir = EVIDENCE_ROOT / f"{run_stamp}-{safe_stage or 'invalid-stage'}"
    evidence_dir.mkdir(parents=True, exist_ok=False)
    summary_path = evidence_dir / "summary.json"
    three_board_requested = args.slave2_id is not None or args.slave2_uf2 is not None
    labels = ("master", "slave1", "slave2") if three_board_requested else (
        "master",
        "slave",
    )
    raw_log_paths = {
        label: (evidence_dir / f"{label}.raw.log").resolve() for label in labels
    }
    print(f"board gate evidence: {evidence_dir.resolve()}", flush=True)

    raw_logs = {
        label: TimestampedRawLog(raw_log_paths[label]) for label in labels
    }
    sessions: dict[str, BoardSession] = {}
    stage_result: dict[str, object] = {
        "master_to_slave": False,
        "slave_to_master": False,
        "queue_drops": 0,
        "recoveries": 0,
        "recovery_limit": len(labels),
    }
    latency_samples: dict[str, list[float]] = {}
    restart_records: list[dict[str, object]] = []
    summary: dict[str, object] = {
        "stage": args.stage,
        "board_ids": {},
        "uf2_sha256": {},
        "directions": {
            "master_to_slave": False,
            "slave_to_master": False,
        },
        "master_to_slave": False,
        "slave_to_master": False,
        "queue_drops": 0,
        "recoveries": 0,
        "restarts": [],
        "latency_ms": {"samples": {}, "summary": {}},
        "boards": {
            label: {"flashes": 0, "flash_retries": 0, "recoveries": 0}
            for label in labels
        },
        "pass": False,
        "complete": False,
        "error": None,
        "raw_log_paths": {
            label: str(raw_log_paths[label]) for label in labels
        },
        "started_at": started_at,
        "ended_at": None,
    }
    return_code = 1
    run_error: str | None = None
    summary_write_error: str | None = None
    try:
        plan = _validate_board_plan(args)
        if args.cycles < 1:
            raise GateError("--cycles must be >= 1")
        if args.cold_reboots_per_board < 0:
            raise GateError("--cold-reboots-per-board must be >= 0")
        if args.watchdog_restarts_per_board < 0:
            raise GateError("--watchdog-restarts-per-board must be >= 0")
        if args.steady_state_seconds < 0:
            raise GateError("--steady-state-seconds must be >= 0")
        if not 1 <= args.bridge_length <= BRIDGE_MAX_LENGTH:
            raise GateError(f"--bridge-length must be in [1, {BRIDGE_MAX_LENGTH}]")
        if args.bridge_repeats < 1:
            raise GateError("--bridge-repeats must be >= 1")
        if args.downlink_loss_every_n < 0 or args.downlink_loss_every_n == 1:
            raise GateError("--downlink-loss-every-n must be 0 or >= 2")
        if args.command_timeout <= 0:
            raise GateError("--command-timeout must be > 0")
        if not safe_stage:
            raise GateError("--stage must contain a path-safe character")
        for item in plan:
            if not item.uf2.is_file():
                raise GateError(f"{item.label} UF2 does not exist: {item.uf2}")

        summary["board_ids"] = {
            item.label: item.device_id for item in plan
        }
        summary["uf2_sha256"] = {
            item.label: _uf2_sha256(item.uf2) for item in plan
        }
        sessions = {
            item.label: BoardSession(
                item.label,
                item.device_id,
                item.uf2,
                args.command_timeout,
                raw_logs[item.label],
                RecoveryBudget(),
            )
            for item in plan
        }
        master = sessions["master"]
        slaves = [sessions[item.label] for item in plan if item.role_id != 0]
        for cycle in range(1, args.cycles + 1):
            _merge_latency_samples(
                latency_samples,
                _run_cycle(
                    master,
                    slaves[0] if len(slaves) == 1 else slaves,
                    cycle,
                    args.cycles,
                    args.bridge_length,
                    args.bridge_repeats,
                ),
            )
            stage_result["master_to_slave"] = True
            stage_result["slave_to_master"] = True
        if len(slaves) != 1 and (
            args.downlink_loss_every_n or args.uplink_ack_loss_once
        ):
            raise GateError(
                "three-board loss-injection options require the dual-board flow"
            )
        if args.downlink_loss_every_n:
            _run_downlink_loss_gate(
                master,
                slaves[0],
                args.bridge_length,
                args.downlink_loss_every_n,
            )
        if args.uplink_ack_loss_once:
            _run_uplink_ack_loss_once_gate(master, slaves[0], args.bridge_length)
        if args.cold_reboots_per_board:
            restart_records.extend(
                _run_cold_reboot_cycles(
                    master,
                    slaves[0] if len(slaves) == 1 else slaves,
                    args.cold_reboots_per_board,
                    args.bridge_length,
                )
            )
        if args.watchdog_restarts_per_board:
            restart_records.extend(
                _run_watchdog_reboot_cycles(
                    master,
                    slaves[0] if len(slaves) == 1 else slaves,
                    args.watchdog_restarts_per_board,
                    args.bridge_length,
                )
            )
        if args.steady_state_seconds:
            _run_steady_state_window(
                master,
                slaves[0] if len(slaves) == 1 else slaves,
                args.steady_state_seconds,
            )
        stage_result["queue_drops"] = sum(
            board.queue_drops for board in sessions.values()
        )
        stage_result["recoveries"] = sum(
            board.recovery_count for board in sessions.values()
        )
        validate_stage_result(stage_result)
        return_code = 0
    except Exception as exc:
        run_error = _exception_text(exc)
    finally:
        active_base_exception = sys.exc_info()[0] is not None
        cleanup_errors: list[str] = []
        cleanup_actions: list[tuple[str, Callable[[], None]]] = [
            (f"{label} board", board.close) for label, board in sessions.items()
        ]
        cleanup_actions.extend(
            (f"{label} raw log", raw_log.close)
            for label, raw_log in raw_logs.items()
        )
        for cleanup_name, cleanup in cleanup_actions:
            try:
                cleanup()
            except Exception as exc:
                cleanup_errors.append(
                    f"cleanup {cleanup_name} failed: {_exception_text(exc)}"
                )

        stage_result["queue_drops"] = sum(
            board.queue_drops for board in sessions.values()
        )
        stage_result["recoveries"] = sum(
            board.recovery_count for board in sessions.values()
        )
        errors = ([run_error] if run_error is not None else []) + cleanup_errors
        if cleanup_errors:
            return_code = 1
        summary["pass"] = return_code == 0
        summary["complete"] = return_code == 0
        summary["error"] = "; ".join(errors) if errors else None
        summary["master_to_slave"] = stage_result["master_to_slave"]
        summary["slave_to_master"] = stage_result["slave_to_master"]
        summary["directions"] = {
            "master_to_slave": stage_result["master_to_slave"],
            "slave_to_master": stage_result["slave_to_master"],
        }
        summary["queue_drops"] = stage_result["queue_drops"]
        summary["recoveries"] = stage_result["recoveries"]
        summary["restarts"] = restart_records
        summary["latency_ms"] = {
            "samples": latency_samples,
            "summary": {
                direction: _latency_summary_ms(samples)
                for direction, samples in latency_samples.items()
                if samples
            },
        }
        summary["boards"] = {
            label: {
                "flashes": sessions[label].flash_count if label in sessions else 0,
                "flash_retries": (
                    sessions[label].flash_retry_count if label in sessions else 0
                ),
                "recoveries": (
                    sessions[label].recovery_count if label in sessions else 0
                ),
            }
            for label in labels
        }
        summary["ended_at"] = _timestamp()
        if not active_base_exception:
            try:
                _write_json_summary(summary_path, summary)
            except Exception as exc:
                summary_write_error = f"summary write failed: {_exception_text(exc)}"
                return_code = 1

    if summary_write_error is not None:
        print(
            f"BOARD_E2E FAIL stage={args.stage} error={summary_write_error} "
            f"evidence={evidence_dir.resolve()}",
            file=sys.stderr,
            flush=True,
        )
        return 1
    if return_code != 0:
        print(
            f"BOARD_E2E FAIL stage={args.stage} error={summary['error']} "
            f"evidence={evidence_dir.resolve()}",
            file=sys.stderr,
            flush=True,
        )
        return 1
    board_counts = " ".join(
        f"{label}_flashes={board.flash_count} "
        f"{label}_flash_retries={board.flash_retry_count} "
        f"{label}_recoveries={board.recovery_count}"
        for label, board in sessions.items()
    )
    print(
        "BOARD_E2E PASS "
        f"stage={args.stage} cycles={args.cycles} length={args.bridge_length} "
        f"bridge_repeats={args.bridge_repeats} "
        f"downlink_loss_every_n={args.downlink_loss_every_n} "
        f"uplink_ack_loss_once={int(args.uplink_ack_loss_once)} "
        f"cold_reboots_per_board={args.cold_reboots_per_board} "
        f"watchdog_restarts_per_board={args.watchdog_restarts_per_board} "
        f"{board_counts} evidence={evidence_dir.resolve()}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
