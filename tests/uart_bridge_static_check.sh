#!/usr/bin/env bash
set -euo pipefail

src="src/uart_bridge.c"
hdr="src/uart_bridge.h"
overlay="boards/xiao_ble_nrf52840.overlay"
conf="Kconfig"
prj="prj.conf"

for required in "$src" "$hdr" "$overlay" "$conf" "$prj"; do
    if [[ ! -f "$required" ]]; then
        echo "missing $required" >&2
        exit 1
    fi
done

rg -q 'bridge[-_]uart' "$overlay"
rg -q 'CONFIG_UART_ASYNC_API=y' "$prj"
rg -q 'UART_RX_RDY|UART_RX_BUF_REQUEST|UART_RX_BUF_RELEASED|UART_RX_DISABLED' "$src"
rg -q 'UART_RX_STOPPED' "$src"
rg -q 'UART_TX_DONE|UART_TX_ABORTED' "$src"
rg -q 'int[[:space:]]+uart_bridge_write_record[[:space:]]*\(' "$hdr"
rg -q 'size_t[[:space:]]+uart_bridge_record_available[[:space:]]*\(' "$hdr"
rg -q 'RB_UART_TX_RECORD_CAPACITY|tx_record_lengths' "$src"
rg -q 'rb_record_queue_push' "$src"
rg -q 'rb_record_queue_pop' "$src"
rg -q 'tx_dma_offset[[:space:]]*\+=[[:space:]]*sent' "$src"
rg -q 'RADIO_BRIDGE_UART_BAUDRATE' "$conf"
rg -q 'RADIO_BRIDGE_UART_RING_SIZE' "$conf"
rg -q 'RADIO_BRIDGE_AGGREGATION_TIMEOUT_US' "$conf"
rg -q 'rx_dma_buf|rx_buf' "$src"
rg -q 'tx_dma_buf|tx_buf' "$src"
rg -q 'k_spin_lock|k_spinlock' "$src"
if rg -q 'k_mutex|k_sem_take|K_FOREVER' "$src"; then
    echo "blocking synchronization in UART adapter is forbidden" >&2
    exit 1
fi

# An aborted transfer reports bytes already sent, not bytes lost.  The
# remaining EasyDMA suffix must be retried; counting evt->data.tx.len as a
# drop would over-report loss and violate DROP_OLDEST semantics.
if rg -q 'tx_drop_bytes[[:space:]]*\+=[[:space:]]*event->data\.tx\.len' "$src"; then
    echo "UART_TX_ABORTED bytes must be retried, not counted as drops" >&2
    exit 1
fi

callback_body="$(awk '/uart_callback|uart_event_handler/{in_fn=1} in_fn{print} in_fn && /^}/{in_fn=0}' "$src")"
if grep -q 'LOG_' <<<"$callback_body"; then
    echo "logging in UART callback is forbidden" >&2
    exit 1
fi

echo "UART bridge static check: PASS"
