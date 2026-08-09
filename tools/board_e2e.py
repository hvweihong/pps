#!/usr/bin/env python3
"""Fixed-ID dual-board end-to-end validation gate."""

import argparse
import os
import re
import sys
import time
from collections import deque
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
SHELL_PROMPT = b"uart:~$ "
FLASH_TIMEOUT_S = 20.0
STATUS_TIMEOUT_S = 30.0
BRIDGE_TIMEOUT_S = 30.0
BRIDGE_MAX_LENGTH = 2048
BOOT_GUARD_CLEAR_MIN_UPTIME_MS = 10000
BOOT_GUARD_CLEAR_TIMEOUT_S = 30.0
HISTORY_MAX_BYTES = 256 * 1024
MASTER_TO_SLAVE_SEED = 49
SLAVE_TO_MASTER_SEED = 114


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


CommandResult = TypeVar("CommandResult")


def _timeout_message(error: CommandTimeout, attempt: int, command: str) -> str:
    tail = error.tail.decode("utf-8", errors="backslashreplace")
    return (
        f"command timeout attempt {attempt}/2 command={command!r} "
        f"receive_tail={tail!r}"
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


def _parse_role(output: str) -> int:
    match = re.search(r"(?m)^role_id = ([0-3]) \((?:persisted|default), reboot\)\s*$", output)
    if match is None:
        raise GateError("param get role_id returned unrecognized output")
    return int(match.group(1))


def _require_zero_drops(output: str, board: str) -> None:
    match = re.search(r"\binput_drop=(\d+)\s+output_drop=(\d+)\b", output)
    if match is None:
        raise GateError(f"{board}: bridge_test stats drop fields are missing")
    input_drop, output_drop = (int(value) for value in match.groups())
    if input_drop != 0 or output_drop != 0:
        raise GateError(
            f"{board}: bridge_test drops are non-zero: "
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
    return latest.group(1) == "2" if latest.group(1) is not None else latest.group(2) == "LOCKED"


def _current_pair_ready(master_output: str, slave_output: str) -> bool:
    return _master_ready(master_output) and _slave_ready(slave_output)


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
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


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
    ):
        self.label = label
        self.device_id = device_id.upper()
        self.uf2 = uf2
        self.command_timeout = command_timeout
        self.raw_log = raw_log
        self.serial_port = None
        self.flash_count = 0
        self.flash_retry_count = 0
        self.recovery_count = 0
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
        return history[mark - self._history_start:]

    def close(self) -> None:
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            finally:
                self.serial_port = None

    def _flash_once_for_current_state(self) -> None:
        no_1200_touch = False
        try:
            resolve_serial(self.device_id)
        except GateError as serial_error:
            if _exact_application_identity_present(self.device_id):
                raise GateError(
                    f"{self.label}: application identity failed closed: {serial_error}"
                ) from serial_error
            try:
                flash_uf2.uf2_disk_for(self.device_id)
            except flash_uf2.FlashError as disk_error:
                raise GateError(
                    f"{self.label}: neither exact application nor UF2 identity is available"
                ) from disk_error
            self.event(f"exact ID already in UF2 mode after: {serial_error}")
            no_1200_touch = True
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
            self.event(
                f"flash exact_id={self.device_id} attempt={attempt}/2 uf2={self.uf2}"
            )
            try:
                self._flash_once_for_current_state()
                self.flash_count += 1
                if attempt == 2:
                    self.flash_retry_count += 1
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
        self._drain()
        self.event("kernel reboot cold requested for role activation")
        self._write(b"kernel reboot cold\r")
        time.sleep(0.2)
        self.close()
        self.connect()


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


def _wait_for_radio_ready(master: BoardSession, slave: BoardSession) -> None:
    deadline = time.monotonic() + max(
        STATUS_TIMEOUT_S, master.command_timeout * 3.0, slave.command_timeout * 3.0
    )
    while time.monotonic() < deadline:
        master_mark = master.history_mark()
        slave_mark = slave.history_mark()
        master.command("kernel uptime")
        slave_output = slave.command("time_sync status")
        master_fresh = master.history_since(master_mark).decode(
            "utf-8", errors="replace"
        )
        slave_fresh = slave.history_since(slave_mark).decode(
            "utf-8", errors="replace"
        )
        if _current_pair_ready(master_fresh, slave_output + slave_fresh):
            master.event("radio ready active_count=1")
            slave.event("time sync ready sync_state=2/LOCKED")
            return
        time.sleep(0.25)
    raise GateError("radio readiness timeout: current active_count=1/LOCKED pair missing")


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
            board.event(f"bridge verify PASS direction={direction} len={length} seed={seed}")
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


def _runtime_drop_values(output: str) -> dict[str, int] | None:
    values: dict[str, int] = {}
    for field in (
        "uart_rx_drop_bytes",
        "uart_tx_drop_bytes",
        "queue_drop_bytes",
    ):
        matches = re.findall(rf"\b{field}=(\d+)\b", output)
        if not matches:
            return None
        values[field] = int(matches[-1])
    return values


def _require_runtime_zero_drops(output: str, board: str) -> None:
    values = _runtime_drop_values(output)
    if values is None:
        raise GateError(f"{board}: one or more runtime drop fields are missing")
    for field, value in values.items():
        if value != 0:
            raise GateError(f"{board}: {field}={value}, expected zero")


def _wait_for_fresh_runtime_zero_drops(board: BoardSession) -> None:
    history_mark = board.history_mark()
    deadline = time.monotonic() + max(STATUS_TIMEOUT_S, board.command_timeout * 3.0)
    while time.monotonic() < deadline:
        board.command("kernel uptime")
        fresh_output = board.history_since(history_mark).decode(
            "utf-8", errors="replace"
        )
        if _runtime_drop_values(fresh_output) is not None:
            _require_runtime_zero_drops(fresh_output, board.label)
            board.event("runtime queue drops PASS uart_rx=0 uart_tx=0 bridge_queue=0")
            return
        time.sleep(0.1)
    raise GateError(f"{board.label}: timed out waiting for fresh runtime drop status")


def _wait_for_boot_guard_clear(board: BoardSession) -> None:
    """Do not issue another cold reset while the persisted boot guard is armed."""
    history_mark = board.history_mark()
    deadline = time.monotonic() + max(
        BOOT_GUARD_CLEAR_TIMEOUT_S, board.command_timeout * 3.0
    )
    while time.monotonic() < deadline:
        output = board.command("kernel uptime")
        match = re.search(r"\bUptime:\s*(\d+)\s*ms\b", output)
        if match is None:
            raise GateError(f"{board.label}: kernel uptime was not parseable")
        uptime_ms = int(match.group(1))
        fresh_output = board.history_since(history_mark).decode(
            "utf-8", errors="replace"
        )
        marker = "radio boot guard cleared after stable startup"
        if uptime_ms >= BOOT_GUARD_CLEAR_MIN_UPTIME_MS and marker in fresh_output:
            board.event(
                "boot guard clear PASS "
                f"uptime_ms={uptime_ms} minimum_ms={BOOT_GUARD_CLEAR_MIN_UPTIME_MS}"
            )
            return
        time.sleep(0.1)
    raise GateError(f"{board.label}: timed out waiting for boot guard clear marker")


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
    history_mark = board.history_mark()
    deadline = time.monotonic() + max(STATUS_TIMEOUT_S, board.command_timeout * 3.0)
    while time.monotonic() < deadline:
        board.command("kernel uptime")
        fresh_output = board.history_since(history_mark).decode(
            "utf-8", errors="replace"
        )
        values = _time_uart_error_values(fresh_output)
        if values is not None:
            nonzero = {field: value for field, value in values.items() if value != 0}
            if nonzero:
                details = " ".join(
                    f"{field}={value}" for field, value in nonzero.items()
                )
                raise GateError(
                    f"{board.label}: time UART input errors or drops {details}"
                )
            board.event("time UART input errors and drops PASS")
            return
        time.sleep(0.1)
    raise GateError(f"{board.label}: timed out waiting for fresh time UART status")


def _uptime_ms(board: BoardSession) -> int:
    output = board.command("kernel uptime")
    match = re.search(r"\bUptime:\s*(\d+)\s*ms\b", output)
    if match is None:
        raise GateError(f"{board.label}: kernel uptime was not parseable")
    return int(match.group(1))


def _run_bidirectional_bridge_gate(
    master: BoardSession, slave: BoardSession, bridge_length: int, label: str
) -> None:
    master.clear_history()
    slave.clear_history()

    command = f"bridge_test inject {bridge_length} {MASTER_TO_SLAVE_SEED}"
    output = master.command(command)
    _expect(
        output,
        f"bridge_test inject ok len={bridge_length} seed={MASTER_TO_SLAVE_SEED}",
        master.label,
        command,
    )
    _wait_for_verify(slave, bridge_length, MASTER_TO_SLAVE_SEED, f"{label}-master-to-slave")

    command = f"bridge_test inject {bridge_length} {SLAVE_TO_MASTER_SEED}"
    output = slave.command(command)
    _expect(
        output,
        f"bridge_test inject ok len={bridge_length} seed={SLAVE_TO_MASTER_SEED}",
        slave.label,
        command,
    )
    _wait_for_verify(master, bridge_length, SLAVE_TO_MASTER_SEED, f"{label}-slave-to-master")

    for board in (master, slave):
        stats = board.command("bridge_test stats")
        _require_zero_drops(stats, board.label)
        board.event("bridge_test stats PASS input_drop=0 output_drop=0")
        _wait_for_fresh_runtime_zero_drops(board)
        _wait_for_fresh_time_uart_zero_errors(board)


def _run_cold_reboot_cycles(
    master: BoardSession, slave: BoardSession, cycles: int, bridge_length: int
) -> None:
    for board, peer in ((master, slave), (slave, master)):
        for cycle in range(1, cycles + 1):
            before_uptime = _uptime_ms(board)
            board.event(
                f"cold reboot board={board.label} cycle={cycle}/{cycles} "
                f"pre_uptime_ms={before_uptime}"
            )
            board.reboot()
            # USB CDC can re-enumerate just before its first post-reset read is
            # reliable.  This bounded settle avoids mistaking that transition
            # for a firmware command timeout/reflash condition.
            time.sleep(1.0)
            after_uptime = _uptime_ms(board)
            if after_uptime >= before_uptime:
                raise GateError(
                    f"{board.label}: cold reboot did not reduce uptime "
                    f"before={before_uptime} after={after_uptime}"
                )
            _wait_for_boot_guard_clear(board)
            _wait_for_radio_ready(master, slave)
            for checked_board in (board, peer):
                _wait_for_fresh_runtime_zero_drops(checked_board)
                _wait_for_fresh_time_uart_zero_errors(checked_board)
            board.event(
                f"cold reboot board={board.label} cycle={cycle}/{cycles} PASS "
                f"post_uptime_ms={after_uptime} peer_ready=PASS"
            )
    _run_bidirectional_bridge_gate(master, slave, bridge_length, "post-cold-reboots")


def _run_steady_state_window(
    master: BoardSession, slave: BoardSession, seconds: float
) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        _wait_for_radio_ready(master, slave)
        for board in (master, slave):
            _wait_for_fresh_runtime_zero_drops(board)
            _wait_for_fresh_time_uart_zero_errors(board)
        time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
    for board in (master, slave):
        board.event(f"steady state PASS seconds={seconds:g}")


def _run_cycle(
    master: BoardSession,
    slave: BoardSession,
    cycle: int,
    cycles: int,
    bridge_length: int,
) -> None:
    master.event(f"cycle={cycle}/{cycles} start")
    slave.event(f"cycle={cycle}/{cycles} start")
    master.flash()
    slave.flash()
    master.connect()
    slave.connect()
    _ensure_role(master, 0)
    _ensure_role(slave, 1)
    master.clear_history()
    slave.clear_history()

    for board in (master, slave):
        uptime = board.command("kernel uptime")
        _expect(uptime, "Uptime:", board.label, "kernel uptime")
        clear = board.command("bridge_test clear")
        _expect(clear, "bridge_test clear ok", board.label, "bridge_test clear")

    _wait_for_radio_ready(master, slave)

    _run_bidirectional_bridge_gate(master, slave, bridge_length, f"cycle-{cycle}")
    for board in (master, slave):
        board.event(f"cycle={cycle}/{cycles} PASS")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, help="stage label used in evidence paths")
    parser.add_argument("--master-id", default=MASTER_ID, help="complete master USB ID")
    parser.add_argument("--slave-id", default=SLAVE_ID, help="complete slave USB ID")
    parser.add_argument("--master-uf2", required=True, type=Path)
    parser.add_argument("--slave-uf2", required=True, type=Path)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument(
        "--cold-reboots-per-board", type=int, default=0,
        help="Sequential cold reboots per board after flashing; waits >=10s guard clear",
    )
    parser.add_argument(
        "--steady-state-seconds", type=float, default=0.0,
        help="Post-gate radio and UART health observation window",
    )
    parser.add_argument("--bridge-length", type=int, default=600)
    parser.add_argument("--command-timeout", type=float, default=10.0)
    return parser


def _validated_id(device_id: str, option: str) -> str:
    if len(device_id) != 16 or re.fullmatch(r"[0-9A-Fa-f]{16}", device_id) is None:
        raise GateError(f"{option} must be a complete 16-hex-digit USB ID")
    return device_id.upper()


def main(argv: Iterable[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        master_id = _validated_id(args.master_id, "--master-id")
        slave_id = _validated_id(args.slave_id, "--slave-id")
        if master_id == slave_id:
            raise GateError("master and slave USB IDs must be distinct")
        if args.cycles < 1:
            raise GateError("--cycles must be >= 1")
        if args.cold_reboots_per_board < 0:
            raise GateError("--cold-reboots-per-board must be >= 0")
        if args.steady_state_seconds < 0:
            raise GateError("--steady-state-seconds must be >= 0")
        if not 1 <= args.bridge_length <= BRIDGE_MAX_LENGTH:
            raise GateError(
                f"--bridge-length must be in [1, {BRIDGE_MAX_LENGTH}]"
            )
        if args.command_timeout <= 0:
            raise GateError("--command-timeout must be > 0")
        for name, uf2 in (("master", args.master_uf2), ("slave", args.slave_uf2)):
            if not uf2.is_file():
                raise GateError(f"{name} UF2 does not exist: {uf2}")

        stage = re.sub(r"[^A-Za-z0-9_.-]+", "-", args.stage).strip("-.")
        if not stage:
            raise GateError("--stage must contain a path-safe character")
        run_stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        evidence_dir = Path("build") / "board-e2e" / f"{run_stamp}-{stage}"
        evidence_dir.mkdir(parents=True, exist_ok=False)
        print(f"board gate evidence: {evidence_dir.resolve()}", flush=True)

        master_log = TimestampedRawLog(evidence_dir / "master.raw.log")
        slave_log = TimestampedRawLog(evidence_dir / "slave.raw.log")
        master = BoardSession(
            "master", master_id, args.master_uf2, args.command_timeout, master_log
        )
        slave = BoardSession(
            "slave", slave_id, args.slave_uf2, args.command_timeout, slave_log
        )
        try:
            for cycle in range(1, args.cycles + 1):
                _run_cycle(master, slave, cycle, args.cycles, args.bridge_length)
            if args.cold_reboots_per_board:
                _run_cold_reboot_cycles(
                    master, slave, args.cold_reboots_per_board, args.bridge_length
                )
            if args.steady_state_seconds:
                _run_steady_state_window(
                    master, slave, args.steady_state_seconds
                )
            print(
                "BOARD_E2E PASS "
                f"stage={args.stage} cycles={args.cycles} length={args.bridge_length} "
                f"cold_reboots_per_board={args.cold_reboots_per_board} "
                f"master_flashes={master.flash_count} slave_flashes={slave.flash_count} "
                f"master_flash_retries={master.flash_retry_count} "
                f"slave_flash_retries={slave.flash_retry_count} "
                f"master_recoveries={master.recovery_count} "
                f"slave_recoveries={slave.recovery_count} "
                f"evidence={evidence_dir.resolve()}",
                flush=True,
            )
            return 0
        except (GateError, CommandTimeout) as exc:
            print(
                f"BOARD_E2E FAIL stage={args.stage} error={exc} "
                f"evidence={evidence_dir.resolve()}",
                file=sys.stderr,
                flush=True,
            )
            return 1
        finally:
            master.close()
            slave.close()
            master_log.close()
            slave_log.close()
    except GateError as exc:
        print(f"BOARD_E2E FAIL error={exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
