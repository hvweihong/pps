#!/usr/bin/env bash
set -euo pipefail

if rg -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y$' prj.conf; then
	echo "SKIP: tests/radio_address_and_rx_state_static_check.sh covers the retired BLE/MPSL application"
	exit 0
fi

python3 - <<'PY'
from pathlib import Path

root = Path.cwd()
radio_c = (root / "src" / "radio_sync.c").read_text()
radio_h = (root / "src" / "radio_sync.h").read_text()
status_c = (root / "src" / "status.c").read_text()

if "nrf_radio_txaddress_set(NRF_RADIO, 0);" not in radio_c:
    raise SystemExit("RADIO common config must explicitly select logical TX address 0")

if "nrf_radio_datawhiteiv_set(NRF_RADIO, channel);" not in radio_c:
    raise SystemExit("RADIO common config must explicitly set whitening IV from the RF channel")

if "NRF_RADIO_INT_ADDRESS_MASK" not in radio_c:
    raise SystemExit("RADIO diagnostics must enable ADDRESS interrupts, not only END")

if "record_rx_address_if_seen" not in radio_c:
    raise SystemExit("RADIO callback must count ADDRESS events independently")

for field in (
    "tx_address",
    "rx_address_mask",
    "radio_datawhiteiv",
    "rxen_events",
    "rx_ready_events",
    "last_rx_radio_state",
):
    if field not in radio_h:
        raise SystemExit(f"missing radio_sync_stats field: {field}")

for token in (
    "tx_addr=%u",
    "rx_addrs=0x%02x",
    "white=%u",
    "rxen=%u",
    "rx_ready=%u",
    "rx_state=%u",
):
    if token not in status_c:
        raise SystemExit(f"status log must include {token}")
PY
