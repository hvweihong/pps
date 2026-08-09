#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include <hal/nrf_power.h>

#include "app_config.h"
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
#define RB_RADIO_BOOT_GUARD_KEY "diag/boot_guard"
#define RB_RADIO_BOOT_GUARD_MAGIC 0x52424744u
#define RB_RADIO_BOOT_GUARD_CLEAR_DELAY_MS 10000

struct radio_boot_guard_load_context {
	uint32_t value;
	bool found;
	int error;
};

static int radio_boot_guard_load_cb(const char *key, size_t len,
				    settings_read_cb read_cb, void *cb_arg,
				    void *param)
{
	struct radio_boot_guard_load_context *context = param;
	ssize_t bytes_read;

	if (settings_name_next(key, NULL) != 0) {
		return 0;
	}
	context->found = true;
	if (len != sizeof(context->value)) {
		context->error = -EINVAL;
		return 1;
	}
	bytes_read = read_cb(cb_arg, &context->value, sizeof(context->value));
	if (bytes_read != sizeof(context->value)) {
		context->error = bytes_read < 0 ? (int)bytes_read : -EIO;
	}
	return 1;
}

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

int main(void)
{
	int ret;
	bool led_heartbeat;
	bool boot_guard_armed = false;
	bool retained_diag_valid;
	int64_t boot_guard_clear_at;
	uint32_t reset_reason = nrf_power_resetreas_get(NRF_POWER);
	struct radio_boot_guard_load_context boot_guard = {0};
	struct rb_radio_retained_diag retained_diag = {0};
	uint32_t role_id, uart_baud, group_id, pps_period_us, status_interval_ms;
	uint32_t pps_width_us, led_period_ms;

	ret = rb_param_config_init();
	if (ret != 0) {
		LOG_ERR("param config init failed: %d", ret);
		return 0;
	}
	ret = settings_load_subtree_direct(RB_RADIO_BOOT_GUARD_KEY,
					 radio_boot_guard_load_cb, &boot_guard);
	if (ret != 0 || boot_guard.error != 0 || boot_guard.found) {
		int load_error = boot_guard.error != 0 ? boot_guard.error : ret;

		retained_diag_valid = radio_transport_retained_diag_get(&retained_diag);
		if (boot_guard.found) {
			ret = settings_delete(RB_RADIO_BOOT_GUARD_KEY);
			if (ret != 0) {
				LOG_ERR("radio boot guard clear failed: %d", ret);
			}
		}
		radio_transport_retained_diag_clear();
		register_usbd_callbacks();
		for (;;) {
			uint8_t history_start;

			LOG_ERR("radio boot guard recovered: reset_reason=0x%08x "
				"load_error=%d guard=0x%08x retained=%u stage=0x%02x "
				"switch=%u current=%u target=%u active=%u result=%d "
				"action=%u event=%u length=%u radio_state=%u "
				"events_disabled=%u hfclkstat=0x%08x primask=%u ipsr=%u "
				"updates=%u",
				reset_reason, load_error, boot_guard.value,
				retained_diag_valid, retained_diag.stage,
				retained_diag.switch_index,
				retained_diag.current_profile,
				retained_diag.target_profile,
				retained_diag.esb_active, retained_diag.last_result,
				retained_diag.runtime_action,
				retained_diag.radio_event_id,
				retained_diag.payload_length,
				retained_diag.radio_state,
				retained_diag.events_disabled,
				retained_diag.hfclkstat, retained_diag.primask,
				retained_diag.ipsr, retained_diag.update_count);
			history_start = (retained_diag.history_next +
				RB_RADIO_RETAINED_HISTORY_SIZE - retained_diag.history_count) %
				RB_RADIO_RETAINED_HISTORY_SIZE;
			for (uint8_t i = 0u; i < retained_diag.history_count; i++) {
				const struct rb_radio_retained_diag_entry *entry =
					&retained_diag.history[(history_start + i) %
						RB_RADIO_RETAINED_HISTORY_SIZE];

				LOG_ERR("radio diag history: seq=%u cycle=%u stage=0x%02x "
					"result=%d action=%u event=%u length=%u state=%u "
					"current=%u target=%u active=%u primask=%u",
					entry->sequence, entry->cycle, entry->stage,
					entry->result, entry->runtime_action,
					entry->radio_event_id, entry->payload_length,
					entry->radio_state, entry->current_profile,
					entry->target_profile, entry->esb_active,
					entry->primask);
				k_sleep(K_MSEC(100));
			}
			k_sleep(K_SECONDS(1));
		}
	}
	ret = settings_save_one(RB_RADIO_BOOT_GUARD_KEY,
				&((uint32_t){RB_RADIO_BOOT_GUARD_MAGIC}),
				sizeof(uint32_t));
	if (ret != 0) {
		register_usbd_callbacks();
		for (;;) {
			LOG_ERR("radio boot guard arm failed: %d; radio disabled", ret);
			k_sleep(K_SECONDS(2));
		}
	}
	boot_guard_armed = true;
	boot_guard_clear_at = k_uptime_get() + RB_RADIO_BOOT_GUARD_CLEAR_DELAY_MS;

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

	ret = rb_param_get_uint32(RB_PARAM_PPS_PERIOD_US, &pps_period_us);
	if (ret != 0) {
		LOG_ERR("failed to read pps_period_us: %d", ret);
		return 0;
	}

	ret = rb_param_get_uint32(RB_PARAM_PPS_WIDTH_US, &pps_width_us);
	if (ret != 0) {
		LOG_ERR("failed to read pps_width_us: %d", ret);
		return 0;
	}

	ret = rb_param_get_uint32(RB_PARAM_STATUS_INTERVAL_MS, &status_interval_ms);
	if (ret != 0) {
		LOG_ERR("failed to read status_interval_ms: %d", ret);
		return 0;
	}

	ret = rb_param_get_bool(RB_PARAM_LED_HEARTBEAT, &led_heartbeat);
	if (ret != 0) {
		LOG_ERR("failed to read led_heartbeat: %d", ret);
		return 0;
	}

	ret = rb_param_get_uint32(RB_PARAM_LED_PERIOD_MS, &led_period_ms);
	if (ret != 0) {
		LOG_ERR("failed to read led_period_ms: %d", ret);
		return 0;
	}

	LOG_INF("=== XIAO nRF52840 Star Radio UART Bridge ===");
	LOG_INF("Role: %s (role_id=%u), UART: %u baud",
		role_id == 0u ? "MASTER" : "SLAVE", role_id, uart_baud);

	ret = watchdog_init();
	if (ret != 0) {
		LOG_WRN("Watchdog init failed: %d (continuing without watchdog)", ret);
	}
	register_usbd_callbacks();

	ret = timebase_init();
	if (ret != 0) {
		LOG_ERR("timebase init failed: %d", ret);
		return 0;
	}
	ret = pps_output_init((uint16_t)pps_width_us);
	if (ret != 0) {
		LOG_ERR("pps init failed: %d", ret);
		return 0;
	}
	if (led_heartbeat) {
		ret = heartbeat_led_start((uint16_t)led_period_ms);
		if (ret != 0) {
			LOG_WRN("heartbeat LED disabled: %d", ret);
		}
	}
	ret = pps_output_start_periodic(timebase_now_us() + 100000u, pps_period_us);
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

	LOG_INF("star bridge: group=%u channel=%u frequency=%uMHz payload=%u",
		group_id,
		(unsigned)rb_channel_index(group_id),
		(unsigned)rb_channel_frequency_mhz(group_id),
		(unsigned)RB_ESB_MAX_PAYLOAD);
	for (;;) {
		status_log_bridge();
		if (boot_guard_armed && k_uptime_get() >= boot_guard_clear_at) {
			ret = settings_delete(RB_RADIO_BOOT_GUARD_KEY);
			if (ret == 0) {
				boot_guard_armed = false;
				LOG_INF("radio boot guard cleared after stable startup");
			} else {
				LOG_ERR("radio boot guard clear failed: %d", ret);
				boot_guard_clear_at = INT64_MAX;
			}
		}
		if (wdt_dev != NULL) {
			wdt_feed(wdt_dev, wdt_channel_id);
		}
		k_sleep(K_MSEC(status_interval_ms));
	}
}
