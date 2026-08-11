from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"
UART_BRIDGE = ROOT / "src/bridge/uart_bridge.c"
UART_BRIDGE_HEADER = ROOT / "src/bridge/uart_bridge.h"


def test_uart_stopped_events_preserve_reason_diagnostics():
    source = UART_BRIDGE.read_text(encoding="utf-8")
    header = UART_BRIDGE_HEADER.read_text(encoding="utf-8")

    assert "uint32_t rx_stopped_events;" in header
    assert "uint32_t rx_stop_reason_mask;" in header
    assert "stats.rx_stopped_events++;" in source
    assert "stats.rx_stop_reason_mask |= event->data.rx_stop.reason;" in source


def test_uart_rx_fault_restart_leaves_isr_context_and_backs_off():
    source = UART_BRIDGE.read_text(encoding="utf-8")
    header = UART_BRIDGE_HEADER.read_text(encoding="utf-8")
    cmake = ROOT_CMAKE.read_text(encoding="utf-8")
    disabled_start = source.index("case UART_RX_DISABLED:")
    disabled_end = source.index("case UART_TX_DONE:", disabled_start)
    disabled_block = source[disabled_start:disabled_end]

    assert '#include "uart_rx_recovery.h"' in source
    assert "src/bridge/uart_rx_recovery.c" in cmake
    assert "K_WORK_DELAYABLE_DEFINE(rx_restart_work," in source
    assert "rb_uart_rx_recovery_next_delay_ms(&rx_recovery)" in source
    assert "k_work_reschedule(&rx_restart_work, K_MSEC(delay_ms))" in source
    assert "schedule_rx_restart();" in disabled_block
    assert "uart_rx_enable(" not in disabled_block
    assert "stats.rx_restart_errors++;" in source
    assert "rb_uart_rx_recovery_reset(&rx_recovery);" in source
    assert "if (rx->len != 0u && wake_callback != NULL)" in source
    assert "uint32_t rx_restart_delay_ms;" in header
