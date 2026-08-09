#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/radio_air_diagnostics_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$repo_root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
radio_c = (root / "src/radio_sync.c").read_text()
radio_h = (root / "src/radio_sync.h").read_text()
status = (root / "src/status.c").read_text()

if "RADIO_BASE_FREQUENCY_MHZ" not in radio_c:
    raise SystemExit("radio code must define the 2400 MHz base frequency")

if "radio_frequency_mhz(channel)" not in radio_c:
    raise SystemExit("radio_configure_common must convert channel offset to MHz")

if "nrf_radio_frequency_set(NRF_RADIO, freq_mhz)" not in radio_c:
    raise SystemExit("nrf_radio_frequency_set must receive absolute MHz, not offset")

for token in [
    "radio_frequency_mhz",
    "last_tx_packet_len",
    "rx_address_events",
    "rx_end_events",
    "rx_bad_length_events",
    "last_rx_packet_len",
    "last_rx_crc_ok",
]:
    if token not in radio_h:
        raise SystemExit(f"radio stats missing diagnostic field: {token}")

for token in [
    "freq=%u",
    "tx_len=%u",
    "rx_addr=%u",
    "rx_end=%u",
    "rx_len=%u",
    "rx_crc_ok=%u",
    "rx_bad_len=%u",
]:
    if token not in status:
        raise SystemExit(f"status log missing diagnostic token: {token}")

if "record_rx_address_if_seen" not in radio_c:
    raise SystemExit("RX diagnostics must count ADDRESS events independently of valid packets")
PY
