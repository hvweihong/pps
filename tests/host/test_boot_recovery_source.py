from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "src/app/main.c"


def test_boot_recovery_does_not_mutate_settings():
    source = MAIN.read_text(encoding="utf-8")
    assert "diag/boot_guard" not in source
    assert "settings_save_one" not in source
    assert "settings_delete" not in source


def test_watchdog_starts_before_parameter_initialization():
    source = MAIN.read_text(encoding="utf-8")
    body = source[source.index("int main(void)") :]
    assert body.index("watchdog_init()") < body.index("rb_param_config_init()")


def test_reset_reason_is_cleared_and_classified():
    source = MAIN.read_text(encoding="utf-8")
    assert "nrf_power_resetreas_clear" in source
    assert "NRF_POWER_RESETREAS_DOG_MASK" in source
    assert "rb_boot_recovery_classify" in source


def test_recovery_log_waits_for_cdc_after_runtime_start():
    source = MAIN.read_text(encoding="utf-8")
    body = source[source.index("int main(void)") :]
    assert "UART_LINE_CTRL_DTR" in source
    assert body.index("bridge_runtime_start()") < body.index("log_boot_recovery(")
    assert "k_sleep(K_MSEC(10))" in source


def test_watchdog_trigger_is_validation_only():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    source = ROOT / "src/validation/boot_validation_shell.c"
    assert "target_sources_ifdef(CONFIG_RADIO_BRIDGE_VALIDATION_CDC" in cmake
    assert "src/validation/boot_validation_shell.c" in cmake
    assert source.is_file()
    text = source.read_text(encoding="utf-8")
    assert "SHELL_CMD_REGISTER(boot_test" in text
    assert "irq_lock()" in text
