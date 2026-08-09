#include "radio_transport.h"

#include <errno.h>
#include <string.h>

#include <esb.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_radio.h>
#include <helpers/nrfx_gppi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/toolchain.h>

#include "bridge_config.h"
#include "timebase.h"

LOG_MODULE_REGISTER(radio_transport, LOG_LEVEL_INF);

#define RB_RADIO_EVENT_QUEUE_SIZE 8u
#define RB_RADIO_RETAINED_DIAG_MAGIC 0x52424447u

static struct rb_radio_transport_config transport_config;
static enum rb_radio_profile current_profile;
static struct k_msgq event_queue;
static char event_queue_buffer[RB_RADIO_EVENT_QUEUE_SIZE * sizeof(struct rb_radio_event)];
static bool initialized;
/* Tracks whether the ESB peripheral has actually completed esb_init() at
 * least once. Calling esb_stop_rx()/esb_disable() before that point is
 * undefined behavior in the ESB library (it can spin waiting for a radio
 * state transition that was never armed) and was observed to hang
 * indefinitely on the very first profile switch. */
static bool esb_active;
static enum rb_radio_profile diagnostic_target_profile;
static uint32_t diagnostic_switch_index;
static uint8_t diagnostic_runtime_action;
static uint8_t diagnostic_radio_event_id;
static uint16_t diagnostic_payload_length;
static struct rb_radio_retained_diag retained_diag __noinit;
static rb_radio_transport_wake_fn wake_callback;
static nrfx_gppi_handle_t radio_capture_handle;
static bool radio_capture_initialized;

static int radio_capture_init(void)
{
	uint32_t address_event;
	uint32_t capture_task;
	int ret;

	if (radio_capture_initialized) {
		return 0;
	}
	address_event = nrf_radio_event_address_get(
		NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	capture_task = timebase_timer_capture_task_address(
		TIMEBASE_RADIO_CAPTURE_CHANNEL);
	if (address_event == 0u || capture_task == 0u) {
		return -EINVAL;
	}
	ret = nrfx_gppi_conn_alloc(address_event, capture_task,
				   &radio_capture_handle);
	if (ret != 0) {
		return ret;
	}
	nrfx_gppi_conn_enable(radio_capture_handle);
	radio_capture_initialized = true;
	return 0;
}

static void radio_boot_stage_mark(enum rb_radio_boot_stage stage, int result)
{
	struct rb_radio_retained_diag_entry *entry;

	retained_diag.magic = 0u;
	retained_diag.magic_inverse = 0u;
	retained_diag.update_count++;
	retained_diag.switch_index = diagnostic_switch_index;
	retained_diag.stage = (uint8_t)stage;
	retained_diag.current_profile = (uint8_t)current_profile;
	retained_diag.target_profile = (uint8_t)diagnostic_target_profile;
	retained_diag.esb_active = esb_active ? 1u : 0u;
	retained_diag.runtime_action = diagnostic_runtime_action;
	retained_diag.radio_event_id = diagnostic_radio_event_id;
	retained_diag.payload_length = diagnostic_payload_length;
	retained_diag.last_result = result;
	retained_diag.radio_state = NRF_RADIO->STATE;
	retained_diag.events_disabled = NRF_RADIO->EVENTS_DISABLED;
	retained_diag.hfclkstat = NRF_CLOCK->HFCLKSTAT;
	retained_diag.primask = __get_PRIMASK();
	retained_diag.ipsr = __get_IPSR();
	entry = &retained_diag.history[retained_diag.history_next];
	*entry = (struct rb_radio_retained_diag_entry) {
		.sequence = retained_diag.update_count,
		.cycle = k_cycle_get_32(),
		.result = result,
		.payload_length = diagnostic_payload_length,
		.stage = (uint8_t)stage,
		.runtime_action = diagnostic_runtime_action,
		.radio_event_id = diagnostic_radio_event_id,
		.radio_state = (uint8_t)retained_diag.radio_state,
		.current_profile = (uint8_t)current_profile,
		.target_profile = (uint8_t)diagnostic_target_profile,
		.esb_active = esb_active ? 1u : 0u,
		.primask = (uint8_t)retained_diag.primask,
	};
	retained_diag.history_next =
		(retained_diag.history_next + 1u) % RB_RADIO_RETAINED_HISTORY_SIZE;
	if (retained_diag.history_count < RB_RADIO_RETAINED_HISTORY_SIZE) {
		retained_diag.history_count++;
	}
	retained_diag.magic_inverse = ~RB_RADIO_RETAINED_DIAG_MAGIC;
	retained_diag.magic = RB_RADIO_RETAINED_DIAG_MAGIC;
}

static void radio_boot_diagnostic_begin(enum rb_radio_profile profile)
{
	diagnostic_switch_index++;
	diagnostic_target_profile = profile;
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_PROFILE_SWITCH, 0);
}

#ifdef CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION
/* Bitmask of rb_frame_type values that should be dropped, and a 1-of-N
 * counter. The cadence counts only frames selected by the mask. */
static uint32_t loss_type_mask;
static uint32_t loss_every_n;
static uint32_t loss_counter;
static uint32_t loss_drop_limit;
static uint32_t loss_drop_count;
static size_t loss_minimum_length;
static bool loss_injector_should_drop(uint8_t frame_type, size_t frame_length)
{
	if (frame_type >= 32u) {
		return false;
	}
	if (loss_type_mask != 0u &&
	    (loss_type_mask & (1u << frame_type)) == 0u) {
		return false;
	}
	if (loss_every_n == 0u) {
		return loss_type_mask != 0u;
	}
	if (frame_length < loss_minimum_length) {
		return false;
	}
	if (loss_drop_limit != 0u && loss_drop_count >= loss_drop_limit) {
		return false;
	}
	loss_counter++;
	if ((loss_counter % loss_every_n) != 0u) {
		return false;
	}
	loss_drop_count++;
	return true;
}

static int queue_injected_tx_success(void)
{
	struct rb_radio_event event = {
		.type = RB_RADIO_EVENT_TX_SUCCESS,
	};
	int ret = k_msgq_put(&event_queue, &event, K_NO_WAIT);

	if (ret == 0 && wake_callback != NULL) {
		wake_callback();
	}
	return ret;
}

void radio_transport_loss_set(uint32_t type_mask, uint32_t every_n)
{
	loss_type_mask = type_mask;
	loss_every_n = every_n;
	loss_counter = 0u;
	loss_drop_limit = 0u;
	loss_drop_count = 0u;
	loss_minimum_length = 0u;
}

void radio_transport_loss_once(uint32_t type_mask, size_t minimum_length)
{
	loss_type_mask = type_mask;
	loss_every_n = 1u;
	loss_counter = 0u;
	loss_drop_limit = 1u;
	loss_drop_count = 0u;
	loss_minimum_length = minimum_length;
}

uint32_t radio_transport_loss_drop_count(void)
{
	return loss_drop_count;
}
#endif /* CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION */

static enum esb_mode profile_mode(enum rb_radio_profile profile)
{
	switch (profile) {
	case RB_RADIO_MASTER_DISCOVERY_PRX:
	case RB_RADIO_SLAVE_GROUP_PRX:
	case RB_RADIO_SLAVE_ASSIGN_PRX:
	case RB_RADIO_SLAVE_ACTIVE_PRX:
		return ESB_MODE_PRX;
	default:
		return ESB_MODE_PTX;
	}
}

static void rb_esb_event_handler(const struct esb_evt *event)
{
	struct rb_radio_event queued = {0};
	struct esb_payload payload = {0};
	int err;

	if (event == NULL) {
		return;
	}
	diagnostic_radio_event_id = (uint8_t)event->evt_id;
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_EVENT_HANDLER, 0);
	queued.address_tick = timebase_capture64(TIMEBASE_RADIO_CAPTURE_CHANNEL);
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_EVENT_CAPTURED, 0);
	queued.attempts = event->tx_attempts;
	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		queued.type = RB_RADIO_EVENT_TX_SUCCESS;
		break;
	case ESB_EVENT_TX_FAILED:
		err = esb_pop_tx();
		if (err != 0) {
			LOG_ERR("failed to release ESB TX payload after TX_FAILED: %d", err);
		}
		queued.type = RB_RADIO_EVENT_TX_FAILED;
		break;
	case ESB_EVENT_RX_RECEIVED:
		queued.type = RB_RADIO_EVENT_RX_RECEIVED;
		while (esb_read_rx_payload(&payload) == 0) {
			queued.pipe = payload.pipe;
			queued.length = payload.length;
			if (queued.length > sizeof(queued.data)) {
				queued.length = sizeof(queued.data);
			}
			memcpy(queued.data, payload.data, queued.length);
#ifdef CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION
			if (queued.length >= 4 &&
				    loss_injector_should_drop(queued.data[3],
							      queued.length)) {
				continue;
			}
#endif
			(void)k_msgq_put(&event_queue, &queued, K_NO_WAIT);
		}
		if (wake_callback != NULL) {
			wake_callback();
		}
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_EVENT_RETURNED, 0);
		return;
	default:
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_EVENT_RETURNED, -ENOMSG);
		return;
	}
	(void)k_msgq_put(&event_queue, &queued, K_NO_WAIT);
	if (wake_callback != NULL) {
		wake_callback();
	}
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_EVENT_RETURNED, 0);
}

static int esb_configure(enum esb_mode mode, const uint8_t *base0,
				 const uint8_t *base1, const uint8_t *prefixes)
{
	struct esb_config config = ESB_DEFAULT_CONFIG;
	uint8_t channel;
	int err;

	config.protocol = ESB_PROTOCOL_ESB_DPL;
	config.mode = mode;
	config.bitrate = ESB_BITRATE_2MBPS;
	config.crc = ESB_CRC_16BIT;
	config.payload_length = RB_ESB_MAX_PAYLOAD;
	config.selective_auto_ack = true;
	config.retransmit_count = 3;
	config.event_handler = rb_esb_event_handler;
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ESB_INIT, 0);
	err = esb_init(&config);
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ESB_INIT_DONE, err);
	if (err != 0) {
		LOG_ERR("esb_init failed: %d", err);
		return err;
	}
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ADDRESS_CONFIG, 0);
	err = esb_set_base_address_0(base0);
	if (err != 0) {
		LOG_ERR("esb_set_base_address_0 failed: %d", err);
		return err;
	}
	err = esb_set_base_address_1(base1);
	if (err != 0) {
		LOG_ERR("esb_set_base_address_1 failed: %d", err);
		return err;
	}
	err = esb_set_prefixes(prefixes, 4);
	if (err != 0) {
		LOG_ERR("esb_set_prefixes failed: %d", err);
		return err;
	}
	channel = (uint8_t)(2u + 2u * rb_channel_index(transport_config.group_id));
	err = esb_set_rf_channel(channel);
	if (err != 0) {
		LOG_ERR("esb_set_rf_channel(%u) failed: %d", channel, err);
		return err;
	}
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ADDRESS_CONFIG_DONE, 0);
	LOG_DBG("ESB configured: mode=%d channel=%u base0=%02x%02x%02x%02x "
		"base1=%02x%02x%02x%02x prefixes=%02x%02x%02x%02x",
		mode, channel,
		base0[0], base0[1], base0[2], base0[3],
		base1[0], base1[1], base1[2], base1[3],
		prefixes[0], prefixes[1], prefixes[2], prefixes[3]);
	if (mode == ESB_MODE_PRX) {
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ESB_START_RX, 0);
		err = esb_start_rx();
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_ESB_START_RX_DONE, err);
		if (err != 0) {
			LOG_ERR("esb_start_rx failed: %d", err);
		}
		return err;
	}
	return 0;
}

int radio_transport_init(const struct rb_radio_transport_config *config)
{
	uint8_t base0[4];
	uint8_t prefixes[4];
	int err;

	if (config == NULL || config->group_id == 0u ||
	    config->group_id == UINT32_MAX) {
		return -EINVAL;
	}
	err = radio_capture_init();
	if (err != 0) {
		LOG_ERR("RADIO ADDRESS capture init failed: %d", err);
		return err;
	}
	transport_config = *config;
	radio_transport_retained_diag_clear();
	diagnostic_switch_index = 0u;
	diagnostic_runtime_action = 0u;
	diagnostic_radio_event_id = 0u;
	diagnostic_payload_length = 0u;
	diagnostic_target_profile = config->master ? RB_RADIO_MASTER_PTX :
		RB_RADIO_SLAVE_GROUP_PRX;
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_TRANSPORT_INIT, 0);
	LOG_INF("radio_transport_init: master=%d group_id=%u",
		config->master, config->group_id);
	k_msgq_init(&event_queue, event_queue_buffer, sizeof(struct rb_radio_event),
			RB_RADIO_EVENT_QUEUE_SIZE);
	initialized = true;

	/* On a cold boot, the very first ESB profile switch has been observed to
	 * hang indefinitely when it goes straight into PRX mode (esb_start_rx()
	 * never returns). Warming the RADIO peripheral up with one throwaway PTX
	 * configuration first (a mode that has never hung) avoids the issue --
	 * this warmup result is discarded and never used to send anything. */
	memcpy(base0, transport_config.addresses.base0, sizeof(base0));
	prefixes[0] = transport_config.addresses.prefix0;
	prefixes[1] = transport_config.addresses.node_prefix[0];
	prefixes[2] = transport_config.addresses.node_prefix[1];
	prefixes[3] = transport_config.addresses.node_prefix[2];
	radio_boot_diagnostic_begin(config->master ? RB_RADIO_MASTER_PTX :
				    RB_RADIO_SLAVE_GROUP_PRX);
	err = esb_configure(ESB_MODE_PTX, base0,
			    transport_config.addresses.base1, prefixes);
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_PROFILE_RETURNED, err);
	if (err != 0) {
		LOG_WRN("ESB PTX warmup failed: %d (continuing anyway)", err);
	} else {
		esb_active = true;
	}

	return radio_transport_set_profile(config->master ? RB_RADIO_MASTER_PTX :
					  RB_RADIO_SLAVE_GROUP_PRX, 0, NULL);
}

int radio_transport_set_profile(enum rb_radio_profile profile,
				uint8_t node_id, const uint8_t temporary_address[5])
{
	uint8_t prefixes[4] = {transport_config.addresses.prefix0,
				      transport_config.addresses.node_prefix[0],
				      transport_config.addresses.node_prefix[1],
				      transport_config.addresses.node_prefix[2]};
	uint8_t base0[4];
	int err;

	if (!initialized || node_id > 3u) {
		return -EINVAL;
	}
	if (temporary_address != NULL &&
	    (profile == RB_RADIO_SLAVE_HELLO_PTX ||
	     profile == RB_RADIO_SLAVE_ASSIGN_PRX ||
	     profile == RB_RADIO_MASTER_ASSIGN_PTX)) {
		memcpy(base0, temporary_address, sizeof(base0));
		prefixes[0] = temporary_address[4];
	} else {
		memcpy(base0, transport_config.addresses.base0, sizeof(base0));
	}
	radio_boot_diagnostic_begin(profile);
	if (esb_active) {
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_STOP_RX, 0);
		err = esb_stop_rx();
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_STOP_RX_DONE, err);
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_DISABLE, 0);
		esb_disable();
		radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_DISABLE_DONE, 0);
	}
	current_profile = profile;
	err = esb_configure(profile_mode(profile), base0,
			    transport_config.addresses.base1, prefixes);
	if (err == 0) {
		esb_active = true;
	}
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_PROFILE_RETURNED, err);
	return err;
}

int radio_transport_send(uint8_t pipe, bool no_ack,
				const uint8_t *data, size_t len)
{
	struct esb_payload payload = {0};
	int err;

	if (!initialized || data == NULL || len == 0u ||
	    len > RB_ESB_MAX_PAYLOAD || pipe > 3u) {
		return -EINVAL;
	}
	payload.pipe = pipe;
	payload.noack = no_ack;
	payload.length = (uint8_t)len;
	memcpy(payload.data, data, len);
	diagnostic_payload_length = (uint16_t)len;
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_SEND, 0);
#ifdef CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION
	/* byte 3 is the frame type in the "RB" common header */
	if (len >= 4 && loss_injector_should_drop(payload.data[3], len)) {
		return queue_injected_tx_success();
	}
#endif
	err = esb_write_payload(&payload);
	radio_boot_stage_mark(RB_RADIO_BOOT_STAGE_SEND_RETURNED, err);
	return err;
}

int radio_transport_queue_ack(uint8_t pipe, const uint8_t *data, size_t len)
{
	return radio_transport_send(pipe, false, data, len);
}

int radio_transport_get_event(struct rb_radio_event *event, k_timeout_t timeout)
{
	if (!initialized || event == NULL) {
		return -EINVAL;
	}
	return k_msgq_get(&event_queue, event, timeout);
}

void radio_transport_set_wake_callback(rb_radio_transport_wake_fn cb)
{
	wake_callback = cb;
}

bool radio_transport_retained_diag_get(struct rb_radio_retained_diag *diag)
{
	if (diag == NULL || retained_diag.magic != RB_RADIO_RETAINED_DIAG_MAGIC ||
	    retained_diag.magic_inverse != ~RB_RADIO_RETAINED_DIAG_MAGIC) {
		return false;
	}
	*diag = retained_diag;
	return true;
}

void radio_transport_retained_diag_clear(void)
{
	memset(&retained_diag, 0, sizeof(retained_diag));
}

void radio_transport_retained_diag_note(enum rb_radio_boot_stage stage,
					uint8_t runtime_action, int result)
{
	diagnostic_runtime_action = runtime_action;
	radio_boot_stage_mark(stage, result);
}
