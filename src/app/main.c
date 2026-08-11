#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include <hal/nrf_power.h>

#include "app_config.h"
#include "boot_recovery.h"
#include "bridge_config.h"
#include "bridge_runtime.h"
#include "heartbeat_led.h"
#include "link_protocol.h"
#include "param_config.h"
#include "pps_output.h"
#include "radio_transport.h"
#include "status.h"
#include "timebase.h"

LOG_MODULE_REGISTER(star_radio_main, LOG_LEVEL_INF);

static const struct device *wdt_dev;
static int wdt_channel_id;
static const struct device *const shell_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));

static int watchdog_init(void)
{
	int ret;
	struct wdt_timeout_cfg wdt_config = {
		.window.min = 0,
		.window.max = 30000, /* 30秒超时 */
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_config);
	if (wdt_channel_id < 0) {
		LOG_ERR("Failed to install watchdog timeout: %d", wdt_channel_id);
		return wdt_channel_id;
	}

	ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret != 0) {
		LOG_ERR("Failed to setup watchdog: %d", ret);
		return ret;
	}

	LOG_INF("Watchdog enabled: 30s timeout, auto-reset on hang");
	return 0;
}

#define DFU_MAGIC_UF2_RESET 0x57u

static void usbd_line_coding_cb(struct usbd_context *const ctx,
					const struct usbd_msg *const msg)
{
	uint32_t rate = 0u;

	ARG_UNUSED(ctx);
	if (msg == NULL || msg->type != USBD_MSG_CDC_ACM_LINE_CODING ||
		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_BAUD_RATE, &rate) != 0 ||
		rate != 1200u) {
		return;
	}

	LOG_INF("1200-baud UF2 reset requested");
	nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC_UF2_RESET);
	sys_reboot(SYS_REBOOT_COLD);
}

static void register_usbd_callbacks(void)
{
	STRUCT_SECTION_FOREACH(usbd_context, usbd_ctx) {
		int ret = usbd_msg_register_cb(usbd_ctx, usbd_line_coding_cb);

		if (ret != 0) {
			LOG_WRN("USB callback registration failed: %d", ret);
		}
	}
}

static void wait_for_cdc_dtr(void)
{
	int64_t deadline = k_uptime_get() + 2000;
	uint32_t dtr = 0u;

	if (!device_is_ready(shell_uart)) {
		return;
	}
	while (k_uptime_get() < deadline) {
		if (uart_line_ctrl_get(shell_uart, UART_LINE_CTRL_DTR, &dtr) == 0 &&
		    dtr != 0u) {
			return;
		}
		if (wdt_dev != NULL) {
			(void)wdt_feed(wdt_dev, wdt_channel_id);
		}
		k_sleep(K_MSEC(10));
	}
}

static void log_boot_recovery(enum rb_boot_recovery_action recovery,
			      uint32_t reset_reason,
			      const struct rb_radio_retained_diag *retained_diag)
{
	uint8_t history_start;

	if (recovery == RB_BOOT_RECOVERY_NORMAL) {
		return;
	}
	wait_for_cdc_dtr();
	if (recovery == RB_BOOT_RECOVERY_WATCHDOG || retained_diag == NULL) {
		LOG_ERR("watchdog reset recovered: reset_reason=0x%08x retained=0",
			reset_reason);
		k_sleep(K_MSEC(10));
		return;
	}

	LOG_ERR("radio watchdog reset recovered: reset_reason=0x%08x "
		"stage=0x%02x switch=%u current=%u target=%u active=%u result=%d "
		"action=%u event=%u length=%u radio_state=%u events_disabled=%u "
		"hfclkstat=0x%08x primask=%u ipsr=%u updates=%u",
		reset_reason, retained_diag->stage, retained_diag->switch_index,
		retained_diag->current_profile, retained_diag->target_profile,
		retained_diag->esb_active, retained_diag->last_result,
		retained_diag->runtime_action, retained_diag->radio_event_id,
		retained_diag->payload_length, retained_diag->radio_state,
		retained_diag->events_disabled, retained_diag->hfclkstat,
		retained_diag->primask, retained_diag->ipsr,
		retained_diag->update_count);
	k_sleep(K_MSEC(10));
	history_start = (retained_diag->history_next + RB_RADIO_RETAINED_HISTORY_SIZE -
			 retained_diag->history_count) % RB_RADIO_RETAINED_HISTORY_SIZE;
	for (uint8_t i = 0u; i < retained_diag->history_count; i++) {
		const struct rb_radio_retained_diag_entry *entry =
			&retained_diag->history[(history_start + i) %
					       RB_RADIO_RETAINED_HISTORY_SIZE];

		LOG_ERR("radio diag history: seq=%u cycle=%u stage=0x%02x result=%d "
			"action=%u event=%u length=%u state=%u current=%u target=%u "
			"active=%u primask=%u",
			entry->sequence, entry->cycle, entry->stage, entry->result,
			entry->runtime_action, entry->radio_event_id,
			entry->payload_length, entry->radio_state,
			entry->current_profile, entry->target_profile,
			entry->esb_active, entry->primask);
		k_sleep(K_MSEC(10));
	}
}

int main(void)
{
	int ret;
	bool retained_diag_valid;
	const uint32_t status_interval_ms = CONFIG_TIME_SYNC_STATUS_INTERVAL_MS;
	uint32_t reset_reason = nrf_power_resetreas_get(NRF_POWER);
	struct rb_radio_retained_diag retained_diag = {0};
	enum rb_boot_recovery_action recovery;
	uint32_t role_id, uart_baud, group_id;

	retained_diag_valid = radio_transport_retained_diag_get(&retained_diag);
	recovery = rb_boot_recovery_classify(reset_reason,
					     NRF_POWER_RESETREAS_DOG_MASK,
					     retained_diag_valid);
	nrf_power_resetreas_clear(NRF_POWER, reset_reason);
	ret = watchdog_init();
	if (ret != 0) {
		LOG_WRN("Watchdog init failed: %d (continuing without watchdog)", ret);
	}
	register_usbd_callbacks();
	radio_transport_retained_diag_clear();

	ret = rb_param_config_init();
	if (ret != 0) {
		LOG_ERR("param config init failed: %d", ret);
		return 0;
	}

	ret = rb_param_get_uint32(RB_PARAM_ROLE_ID, &role_id);
	if (ret != 0) {
		LOG_ERR("failed to read role_id: %d", ret);
		return 0;
	}

	ret = rb_param_get_uint32(RB_PARAM_UART_BAUDRATE, &uart_baud);
	if (ret != 0) {
		LOG_ERR("failed to read uart_baudrate: %d", ret);
		return 0;
	}
	ret = rb_param_get_uint32(RB_PARAM_GROUP_ID, &group_id);
	if (ret != 0) {
		LOG_ERR("failed to read group_id: %d", ret);
		return 0;
	}

	LOG_INF("=== XIAO nRF52840 Star Radio UART Bridge ===");
	LOG_INF("Role: %s (role_id=%u), UART: %u baud",
		role_id == 0u ? "MASTER" : "SLAVE", role_id, uart_baud);

	ret = timebase_init();
	if (ret != 0) {
		LOG_ERR("timebase init failed: %d", ret);
		return 0;
	}
	ret = pps_output_init(CONFIG_TIME_SYNC_PPS_WIDTH_US);
	if (ret != 0) {
		LOG_ERR("pps init failed: %d", ret);
		return 0;
	}
	ret = heartbeat_led_start(IS_ENABLED(CONFIG_TIME_SYNC_LED_HEARTBEAT),
				  TIME_SYNC_LED_HEARTBEAT_PERIOD_MS_VALUE);
	if (ret != 0) {
		LOG_WRN("heartbeat LED unavailable: %d", ret);
	}
	ret = pps_output_start_periodic(timebase_now_us() + 100000u, 1000000u);
	if (ret != 0) {
		LOG_ERR("pps periodic start failed: %d", ret);
		return 0;
	}
	ret = bridge_runtime_init();
	if (ret != 0) {
		LOG_ERR("bridge init failed: %d", ret);
		return 0;
	}
	ret = bridge_runtime_start();
	if (ret != 0) {
		LOG_ERR("bridge start failed: %d", ret);
		return 0;
	}
	log_boot_recovery(recovery, reset_reason,
			  retained_diag_valid ? &retained_diag : NULL);

	LOG_INF("star bridge: group=%u channel=%u frequency=%uMHz payload=%u",
		group_id,
		(unsigned)rb_channel_index(group_id),
		(unsigned)rb_channel_frequency_mhz(group_id),
		(unsigned)RB_ESB_MAX_PAYLOAD);
	for (;;) {
		status_log_bridge();
		if (wdt_dev != NULL) {
			wdt_feed(wdt_dev, wdt_channel_id);
		}
		k_sleep(K_MSEC(status_interval_ms));
	}
}
