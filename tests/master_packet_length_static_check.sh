#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
radio = (root / "src/radio_sync.c").read_text()

match = re.search(
    r"static int encode_tx_packet\(uint64_t master_tx_tick\)\n\{(?P<body>.*?)\n\}",
    radio,
    re.S,
)
if not match:
    raise SystemExit("missing encode_tx_packet")

body = match.group("body")
if "tx_packet[0] = SYNC_BEACON_WIRE_SIZE;" not in body:
    raise SystemExit("periodic master TX must set the RADIO length byte")

if body.index("tx_packet[0] = SYNC_BEACON_WIRE_SIZE;") > body.index("sync_beacon_encode"):
    raise SystemExit("RADIO length byte should be set before encoding/arming TX")
PY
