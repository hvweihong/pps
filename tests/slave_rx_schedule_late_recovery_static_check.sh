#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/slave_rx_schedule_late_recovery_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

python3 - <<'PY'
from pathlib import Path
import re

main = (Path.cwd() / "src" / "main.c").read_text()

late_branch = re.search(
    r"}\s*else\s+if\s*\(ret\s*==\s*-ETIME\)\s*\{(?P<body>.*?)\}\s*else\s*\{",
    main,
    re.S,
)
if not late_branch:
    raise SystemExit("missing explicit RX schedule -ETIME branch")

body = late_branch.group("body")

if "slave_note_missed_rx_window" not in body:
    raise SystemExit("RX schedule -ETIME must use the same recovery path as missed windows")

if "next_rx_window = now + slave_rx_retry_gap_us()" in body:
    raise SystemExit("RX schedule -ETIME must not keep the previous mode with a short retry gap")

if "rx_window_pending = false" not in body:
    raise SystemExit("RX schedule -ETIME must leave no pending RX window")
PY
