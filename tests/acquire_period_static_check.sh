#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
kconfig = (root / "Kconfig").read_text()
main = (root / "src/main.c").read_text()
readme = (root / "README.md").read_text()

def default_for(symbol: str) -> int:
    match = re.search(
        rf"config {symbol}\n(?:\t.*\n)*?\tdefault ([0-9]+)",
        kconfig,
    )
    if not match:
        raise SystemExit(f"missing default for {symbol}")
    return int(match.group(1))

sync_interval = default_for("TIME_SYNC_INTERVAL_US")
acquire_window = default_for("TIME_SYNC_RX_ACQUIRE_WINDOW_US")
acquire_gap = default_for("TIME_SYNC_RX_ACQUIRE_PERIOD_US")
locked_pre = default_for("TIME_SYNC_RX_LOCKED_PRE_MARGIN_US")
locked_post = default_for("TIME_SYNC_RX_LOCKED_POST_MARGIN_US")
locked_guard = default_for("TIME_SYNC_RX_WINDOW_GUARD_US")

if acquire_window < sync_interval:
	raise SystemExit(
		"acquire window must cover a full sync interval before BLE starts"
	)

if acquire_gap >= sync_interval:
    raise SystemExit(
        "acquire gap must be shorter than the sync interval so wide windows "
        "cannot remain phase-blind"
    )

if "slave_rx_window_length_us" not in main:
    raise SystemExit(
        "slave RX scheduling must cap acquire_window + arm_ahead before "
        "requesting an MPSL timeslot"
    )

if "RADIO_SYNC_RX_RECOVERY_WINDOW" not in main:
    raise SystemExit("slave must widen RX before falling back to acquire")

if locked_pre + locked_post + locked_guard >= acquire_window // 10:
    raise SystemExit(
        "locked RX window must be much shorter than acquire window"
	)

expected = f"| `CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US` | acquire RX 窗口间隔 | `{acquire_gap}` |"
if expected not in readme:
    raise SystemExit("README acquire gap default is not in sync with Kconfig")
PY
