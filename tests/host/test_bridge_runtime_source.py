from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
KCONFIG = ROOT / "Kconfig"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
BRIDGE_TEST_CMAKE = ROOT / "tests/unit/bridge/CMakeLists.txt"
RUNTIME = ROOT / "src/bridge/bridge_runtime.c"
RUNTIME_HEADER = ROOT / "src/bridge/bridge_runtime.h"
RADIO_TRANSPORT = ROOT / "src/radio/radio_transport.c"
RADIO_TRANSPORT_HEADER = ROOT / "src/radio/radio_transport.h"
STATUS = ROOT / "src/app/status.c"
VALIDATION_SHELL = ROOT / "src/validation/bridge_validation_shell.c"


def test_bridge_thread_preempts_shell_but_blocks_between_bursts():
    source = RUNTIME.read_text(encoding="utf-8")
    start = source.index("k_thread_create(&bridge_thread")
    creation = source[start : source.index("started = true", start)]
    thread_start = source.index("static void bridge_thread_fn")
    thread_body = source[
        thread_start : source.index("int bridge_runtime_init", thread_start)
    ]

    assert "K_PRIO_PREEMPT(CONFIG_NUM_PREEMPT_PRIORITIES - 2)" in creation
    assert "K_LOWEST_APPLICATION_THREAD_PRIO" not in creation
    assert "k_msgq_get(&bridge_wake_msgq, &wake, K_USEC(100))" in thread_body


def test_runtime_passes_fixed_role_to_scheduler_and_radio():
    source = RUNTIME.read_text(encoding="utf-8")
    scheduler_config = source[source.index("config = (struct rb_scheduler_config)") :]
    radio_config = source[source.index("radio_config =") :]

    assert ".node_id = (uint8_t)role_id" in scheduler_config
    assert ".node_id = (uint8_t)role_id" in radio_config
    assert "radio_transport_set_profile" not in source


def test_runtime_replaces_ack_only_when_scheduler_requests_it():
    source = RUNTIME.read_text(encoding="utf-8")

    assert "radio_transport_replace_ack" in source
    assert "action->replace_ack" in source


def test_radio_event_queue_is_bounded_and_observable():
    source = RADIO_TRANSPORT.read_text(encoding="utf-8")
    header = RADIO_TRANSPORT_HEADER.read_text(encoding="utf-8")

    assert "#define RB_RADIO_EVENT_QUEUE_SIZE 32u" in source
    assert "static int radio_event_enqueue(" in source
    assert source.count("k_msgq_put(") == 1
    assert "atomic_inc(&event_drop_count)" in source
    assert "atomic_set(&event_drop_count, 0)" in source
    assert "radio_transport_event_drop_count(void)" in header
    assert "radio_transport_event_drop_count(void)" in source


def test_on_demand_validation_stats_include_runtime_drop_fields():
    header = RUNTIME_HEADER.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    source = VALIDATION_SHELL.read_text(encoding="utf-8")

    assert '"uart_rx_drop_bytes=%llu uart_tx_drop_bytes=%llu "' in source
    assert '"uart_rx_stopped_events=%u uart_rx_stop_reason_mask=%u "' in source
    assert '"uart_rx_disabled_events=%u uart_rx_restart_errors=%u "' in source
    assert '"uart_rx_restart_delay_ms=%u "' in source
    assert '"queue_drop_bytes=%llu "' in source
    assert '"radio_event_drop_count=%u "' in source
    assert "uart->rx_drop_bytes" in source
    assert "uart->tx_drop_bytes" in source
    assert "uart->rx_disabled_events" in source
    assert "uart->rx_restart_errors" in source
    assert "uart->rx_stopped_events" in source
    assert "uart->rx_stop_reason_mask" in source
    assert "uart->rx_restart_delay_ms" in source
    assert "bridge.queue_drop_bytes" in source
    assert "uint32_t radio_event_drop_count;" in header
    assert (
        "stats->radio_event_drop_count = radio_transport_event_drop_count();"
        in runtime
    )
    assert "bridge.radio_event_drop_count" in source
    assert '"bridge_test time_uart rx_drop_bytes=%llu "' in source
    assert "time_uart->rx_drop_bytes" in source
    assert "pps_input_drop_count()" in source


def test_obsolete_discovery_configuration_is_removed():
    source = KCONFIG.read_text(encoding="utf-8")

    assert "config RADIO_BRIDGE_RESPONSE_SLOT_COUNT" not in source
    assert "config RADIO_BRIDGE_RESPONSE_SLOT_US" not in source
    assert "config RADIO_BRIDGE_ASSIGNMENT_WINDOW_US" not in source


def test_obsolete_repair_window_is_removed():
    assert not (ROOT / "src/bridge/link_window.c").exists()
    assert not (ROOT / "src/bridge/link_window.h").exists()
    assert not (ROOT / "tests/unit/bridge/src/test_link_window.c").exists()
    assert "link_window" not in ROOT_CMAKE.read_text(encoding="utf-8")
    assert "link_window" not in BRIDGE_TEST_CMAKE.read_text(encoding="utf-8")


def test_status_interval_defaults_to_ten_seconds():
    source = KCONFIG.read_text(encoding="utf-8")
    start = source.index("config TIME_SYNC_STATUS_INTERVAL_MS")
    block = source[start : source.index("\nconfig ", start + 1)]

    assert "default 10000" in block
    assert "range 1000 60000" in block


def test_periodic_status_uses_fixed_link_fields():
    header = RUNTIME_HEADER.read_text(encoding="utf-8")
    source = STATUS.read_text(encoding="utf-8")

    for field in (
        "active_mask",
        "node1_ack_age_us",
        "node2_ack_age_us",
        "node3_ack_age_us",
        "node1_poll_failures",
        "node2_poll_failures",
        "node3_poll_failures",
        "invalid_node_packets",
        "slave_session",
    ):
        assert field in header
        assert field in source

    assert "suspect_count" not in header
    assert "suspect_count" not in source
    assert "discovery_hello_count" not in header
    assert "discovery_hello_count" not in source


def test_validation_shell_exposes_whole_record_pair_verifier():
    header = RUNTIME_HEADER.read_text(encoding="utf-8")
    source = VALIDATION_SHELL.read_text(encoding="utf-8")

    assert "bridge_runtime_validation_verify_pair" in header
    assert 'SHELL_CMD_ARG(verify_pair' in source
    assert "bridge_test verify_pair <len1> <seed1> <len2> <seed2>" in source


def test_validation_shell_exposes_uart_paced_injection():
    header = RUNTIME_HEADER.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    source = VALIDATION_SHELL.read_text(encoding="utf-8")

    assert "bridge_runtime_validation_inject_uart" in header
    assert "bridge_runtime_validation_inject_uart" in runtime
    assert "SHELL_CMD_ARG(inject_uart" in source
    assert "bridge_test inject_uart <len> <seed>" in source


def test_on_demand_stats_report_each_fixed_node_record_separately():
    source = VALIDATION_SHELL.read_text(encoding="utf-8")

    for node_id in (1, 2, 3):
        assert f"node{node_id}_records=%llu" in source
        assert f"node{node_id}_bytes=%llu" in source
        assert f"node{node_id}_record_drop=%llu" in source
