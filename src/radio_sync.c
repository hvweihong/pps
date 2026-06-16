#include "radio_sync.h"

#include "app_config.h"
#include "timebase.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <mpsl_timeslot.h>

#define RADIO_PACKET_SIZE (SYNC_BEACON_WIRE_SIZE + 1u)
#define RADIO_ADDRESS_BASE 0x89abcdefu
#define RADIO_ADDRESS_PREFIX 0x45u
#define RADIO_CAPTURE_CHANNEL 3u
#define RADIO_TX_ARM_AHEAD_US 160u
#define RADIO_TX_DEFAULT_ARM_AHEAD_US 5000u
#define RADIO_TX_COMPLETE_TIMEOUT_US 1500u
#define RADIO_TX_REQUEST_TIMEOUT_MARGIN_US 10000u
#define RADIO_TX_SLOT_GUARD_US 80u
#define RADIO_TIMESLOT_MIN_LENGTH_US 100u
#define RADIO_DISABLE_WAIT_US 100u
#define RADIO_IRQ_MASK (NRF_RADIO_INT_END_MASK)
#define TIMESLOT_END_TIMER_CHANNEL NRF_TIMER_CC_CHANNEL0
#define TIMESLOT_TX_TIMEOUT_CHANNEL NRF_TIMER_CC_CHANNEL1
#define TIMESLOT_CAPTURE_CHANNEL NRF_TIMER_CC_CHANNEL2
#define TIMESLOT_TXEN_TIMER_CHANNEL NRF_TIMER_CC_CHANNEL3

enum timeslot_mode {
	TIMESLOT_MODE_IDLE,
	TIMESLOT_MODE_RX,
	TIMESLOT_MODE_TX,
};

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static uint8_t tx_packet[RADIO_PACKET_SIZE] __aligned(4);
static struct sync_beacon pending_tx_beacon;
#endif
static uint8_t rx_packet[RADIO_PACKET_SIZE] __aligned(4);
static struct radio_sync_rx pending_rx;
static uint32_t configured_network_id;
static uint8_t configured_channel;
static struct radio_sync_stats stats;
static nrfx_gppi_handle_t radio_capture_handle;
static bool radio_capture_initialized;
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static nrfx_gppi_handle_t radio_txen_handle;
static bool radio_txen_initialized;
#endif
static mpsl_timeslot_session_id_t timeslot_session_id;
static bool timeslot_session_opened;
static mpsl_timeslot_request_t timeslot_request_earliest;
static mpsl_timeslot_request_t timeslot_request_normal;
static mpsl_timeslot_signal_return_param_t timeslot_return;
static volatile enum timeslot_mode requested_mode = TIMESLOT_MODE_IDLE;
static volatile bool rx_enabled;
static atomic_t rx_state;
static uint64_t active_timeslot_ref_tick;
static uint32_t active_timeslot_ref_count;
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static atomic_t tx_done;
static atomic_t tx_result;
static uint64_t requested_txen_tick;
static uint64_t requested_address_tick;
static uint32_t requested_txen_to_address_us;
#endif

static uint32_t timeslot_length_us(void)
{
	if (TIME_SYNC_TIMESLOT_LENGTH_US_VALUE < RADIO_TIMESLOT_MIN_LENGTH_US) {
		return RADIO_TIMESLOT_MIN_LENGTH_US;
	}

	return TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
}

static uint32_t timeslot_end_margin_us(void)
{
	uint32_t length = timeslot_length_us();
	uint32_t margin = TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE;

	if (margin >= length) {
		return length / 2u;
	}

	return margin;
}

static uint32_t timeslot_cleanup_at_us(void)
{
	return timeslot_length_us() - timeslot_end_margin_us();
}

static void timeslot_requests_init(void)
{
	uint32_t length = timeslot_length_us();

	timeslot_request_earliest = (mpsl_timeslot_request_t) {
		.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
		.params.earliest = {
			.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
			.priority = MPSL_TIMESLOT_PRIORITY_HIGH,
			.length_us = length,
			.timeout_us = TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE,
		},
	};

	timeslot_request_normal = (mpsl_timeslot_request_t) {
		.request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL,
		.params.normal = {
			.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
			.priority = MPSL_TIMESLOT_PRIORITY_HIGH,
			.distance_us = length,
			.length_us = length,
		},
	};
}

static void radio_disable_now(void)
{
	nrf_radio_int_disable(NRF_RADIO, RADIO_IRQ_MASK);
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
}

static void radio_disable_and_wait(void)
{
	uint32_t start;

	radio_disable_now();

	if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_DISABLED) {
		return;
	}

	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	start = nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);

	do {
		if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED) ||
		    nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_DISABLED) {
			nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
			return;
		}

		nrf_timer_task_trigger(MPSL_TIMER0,
				       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	} while ((uint32_t)(nrf_timer_cc_get(MPSL_TIMER0,
					     TIMESLOT_CAPTURE_CHANNEL) -
			    start) < RADIO_DISABLE_WAIT_US);
}

static void radio_configure_common(uint8_t channel)
{
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_frequency_set(NRF_RADIO, channel);
	nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
	nrf_radio_base0_set(NRF_RADIO, RADIO_ADDRESS_BASE);
	nrf_radio_prefix0_set(NRF_RADIO, RADIO_ADDRESS_PREFIX);
	nrf_radio_rxaddresses_set(NRF_RADIO, 0x01);

	const nrf_radio_packet_conf_t packet_conf = {
		.lflen = 8,
		.s0len = 0,
		.s1len = 0,
#if defined(RADIO_PCNF0_S1INCL_Msk)
		.s1incl = false,
#endif
#if defined(RADIO_PCNF0_CILEN_Msk)
		.cilen = 0,
#endif
#if defined(RADIO_PCNF0_PLEN_Msk)
		.plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT,
#endif
#if defined(RADIO_PCNF0_CRCINC_Msk)
		.crcinc = false,
#endif
#if defined(RADIO_PCNF0_TERMLEN_Msk)
		.termlen = 0,
#endif
		.maxlen = SYNC_BEACON_WIRE_SIZE,
		.statlen = 0,
		.balen = 4,
		.big_endian = false,
		.whiteen = true,
	};

	nrf_radio_packet_configure(NRF_RADIO, &packet_conf);
	nrf_radio_crc_configure(NRF_RADIO, 3, NRF_RADIO_CRC_ADDR_SKIP, 0x00065b);
	nrf_radio_crcinit_set(NRF_RADIO, 0x555555);
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_int_enable(NRF_RADIO, RADIO_IRQ_MASK);
}

static int radio_capture_init(void)
{
	uint32_t address_event =
		nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	uint32_t capture_task =
		nrf_timer_task_address_get(
			MPSL_TIMER0,
			nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	int ret;

	ret = nrfx_gppi_conn_alloc(address_event, capture_task,
				   &radio_capture_handle);
	if (ret != 0) {
		return ret;
	}

	nrfx_gppi_conn_disable(radio_capture_handle);
	radio_capture_initialized = true;

	return 0;
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static int radio_txen_trigger_init(void)
{
	uint32_t compare_event =
		nrf_timer_event_address_get(
			MPSL_TIMER0,
			nrf_timer_compare_event_get(TIMESLOT_TXEN_TIMER_CHANNEL));
	uint32_t txen_task =
		nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_TXEN);
	int ret;

	ret = nrfx_gppi_conn_alloc(compare_event, txen_task,
				   &radio_txen_handle);
	if (ret != 0) {
		return ret;
	}

	nrfx_gppi_conn_disable(radio_txen_handle);
	radio_txen_initialized = true;

	return 0;
}
#endif

static void arm_slot_end_timer(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_cc_set(MPSL_TIMER0, TIMESLOT_END_TIMER_CHANNEL,
			 timeslot_cleanup_at_us());
	nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
}

static void timeslot_timer_configure(void)
{
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(MPSL_TIMER0, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(MPSL_TIMER0, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(
		MPSL_TIMER0,
		NRF_TIMER_PRESCALER_CALCULATE(
			NRF_TIMER_BASE_FREQUENCY_GET(MPSL_TIMER0),
			TIMEBASE_TICKS_PER_SEC));
	nrf_timer_shorts_set(MPSL_TIMER0, 0);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_CAPTURE_CHANNEL));
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_TXEN_TIMER_CHANNEL));
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_START);
	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	active_timeslot_ref_count =
		nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);
	active_timeslot_ref_tick = timebase_now_us();
}

static void clear_slot_end_timer(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static void clear_tx_timeout_timer(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
}

static void clear_txen_timer(void)
{
	nrf_timer_int_disable(
		MPSL_TIMER0,
		nrf_timer_compare_int_get(TIMESLOT_TXEN_TIMER_CHANNEL));
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_TXEN_TIMER_CHANNEL));
}

static void complete_tx_from_callback(int result)
{
	atomic_set(&tx_result, result);
	atomic_set(&tx_done, 1);
}
#endif

static void prepare_rx_in_timeslot(void)
{
	rx_packet[0] = 0;
	memset(&rx_packet[1], 0, SYNC_BEACON_WIRE_SIZE);
	radio_configure_common(configured_channel);
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK |
			     NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
	nrf_radio_packetptr_set(NRF_RADIO, rx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	if (radio_capture_initialized) {
		nrfx_gppi_conn_enable(radio_capture_handle);
	}

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static int encode_tx_packet(uint64_t master_tx_tick)
{
	struct sync_beacon wire_beacon = pending_tx_beacon;

	wire_beacon.master_tx_tick = master_tx_tick;
	return sync_beacon_encode(&wire_beacon, &tx_packet[1],
				  SYNC_BEACON_WIRE_SIZE);
}

static int prepare_tx_in_timeslot(void)
{
	uint64_t now = timebase_now_us();
	uint64_t txen_tick = requested_txen_tick;
	uint32_t slot_now;
	uint64_t slot_txen;
	uint32_t tx_timeout_us;

	if (!radio_txen_initialized ||
	    txen_tick <= now + RADIO_TX_ARM_AHEAD_US) {
		stats.tx_timing_errors++;
		return -ETIME;
	}

	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	slot_now = nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);
	slot_txen = (uint64_t)slot_now + (txen_tick - now);

	if (slot_txen + RADIO_TX_COMPLETE_TIMEOUT_US + RADIO_TX_SLOT_GUARD_US >=
	    timeslot_cleanup_at_us()) {
		stats.tx_timing_errors++;
		return -ETIME;
	}

	radio_configure_common(configured_channel);
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK);
	nrf_radio_packetptr_set(NRF_RADIO, tx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	if (radio_capture_initialized) {
		nrfx_gppi_conn_enable(radio_capture_handle);
	}

	requested_address_tick = txen_tick + requested_txen_to_address_us;
	(void)encode_tx_packet(requested_address_tick);
	nrf_timer_cc_set(MPSL_TIMER0, TIMESLOT_TXEN_TIMER_CHANNEL,
			 (uint32_t)slot_txen);
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_TXEN_TIMER_CHANNEL));
	nrfx_gppi_conn_enable(radio_txen_handle);
	tx_timeout_us = (uint32_t)slot_txen + RADIO_TX_COMPLETE_TIMEOUT_US;
	nrf_timer_cc_set(MPSL_TIMER0, TIMESLOT_TX_TIMEOUT_CHANNEL, tx_timeout_us);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
	nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);

	return 0;
}
#endif

static void handle_radio_end(void)
{
	if (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
		return;
	}

	uint64_t timestamp;
	uint32_t captured_slot_tick =
		nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);
	uint32_t ref_age_us = captured_slot_tick - active_timeslot_ref_count;

	timestamp = active_timeslot_ref_tick + ref_age_us;

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

	if (requested_mode == TIMESLOT_MODE_TX) {
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (radio_txen_initialized) {
			nrfx_gppi_conn_disable(radio_txen_handle);
		}
		clear_txen_timer();
		if (radio_capture_initialized) {
			nrfx_gppi_conn_disable(radio_capture_handle);
		}
		stats.tx_packets++;
		stats.last_tx_error_us =
			(int32_t)((int64_t)timestamp -
				  (int64_t)requested_address_tick);
		stats.last_tx_ref_age_us = ref_age_us;
		complete_tx_from_callback(0);
#endif
		radio_disable_now();
		return;
	}

	if (rx_packet[0] == SYNC_BEACON_WIRE_SIZE &&
	    nrf_radio_crc_status_check(NRF_RADIO)) {
		struct sync_beacon beacon;
		int ret = sync_beacon_decode(&rx_packet[1], SYNC_BEACON_WIRE_SIZE,
					     configured_network_id, &beacon);

		if (ret == 0) {
			if (atomic_cas(&rx_state, 0, 2)) {
				pending_rx.beacon = beacon;
				pending_rx.local_rx_tick = timestamp;
				atomic_set(&rx_state, 1);
				stats.rx_packets++;
				stats.last_rx_ref_age_us = ref_age_us;
			} else {
				stats.busy_errors++;
			}
		} else {
			stats.rx_decode_errors++;
		}
	} else if (rx_packet[0] == SYNC_BEACON_WIRE_SIZE) {
		stats.rx_crc_errors++;
	}
}

static mpsl_timeslot_signal_return_param_t *request_next_from_callback(
	mpsl_timeslot_request_t *request)
{
	timeslot_return.params.request.p_next = request;
	timeslot_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
	return &timeslot_return;
}

static mpsl_timeslot_signal_return_param_t *end_from_callback(void)
{
	timeslot_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
	return &timeslot_return;
}

static mpsl_timeslot_signal_return_param_t *none_from_callback(void)
{
	timeslot_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	return &timeslot_return;
}

static mpsl_timeslot_signal_return_param_t *timeslot_start_action(void)
{
	timeslot_timer_configure();
	arm_slot_end_timer();

	if (requested_mode == TIMESLOT_MODE_TX) {
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		int ret = prepare_tx_in_timeslot();

		if (ret == 0) {
			return none_from_callback();
		}

		complete_tx_from_callback(ret);
		radio_disable_and_wait();
		return end_from_callback();
#else
		return end_from_callback();
#endif
	}

	if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
		prepare_rx_in_timeslot();
		return none_from_callback();
	}

	return end_from_callback();
}

static mpsl_timeslot_signal_return_param_t *timeslot_timer_action(void)
{
	if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1)) {
		nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);

		handle_radio_end();

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (requested_mode == TIMESLOT_MODE_TX && atomic_get(&tx_done) != 0) {
			requested_mode = TIMESLOT_MODE_IDLE;
			clear_txen_timer();
			radio_disable_and_wait();
			return end_from_callback();
		}

		if (requested_mode == TIMESLOT_MODE_TX && atomic_get(&tx_done) == 0) {
			stats.busy_errors++;
			stats.tx_timeout_errors++;
			complete_tx_from_callback(-ETIMEDOUT);
			if (radio_txen_initialized) {
				nrfx_gppi_conn_disable(radio_txen_handle);
			}
			clear_txen_timer();
			radio_disable_and_wait();
			return end_from_callback();
		}
#endif
	}

	if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0)) {
		clear_slot_end_timer();
		handle_radio_end();
		radio_disable_and_wait();

		if (radio_capture_initialized) {
			nrfx_gppi_conn_disable(radio_capture_handle);
		}
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (radio_txen_initialized) {
			nrfx_gppi_conn_disable(radio_txen_handle);
		}
		clear_txen_timer();
		clear_tx_timeout_timer();
#endif

		if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
			return request_next_from_callback(&timeslot_request_normal);
		}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (requested_mode == TIMESLOT_MODE_TX && atomic_get(&tx_done) == 0) {
			stats.busy_errors++;
			stats.tx_timeout_errors++;
			complete_tx_from_callback(-ETIMEDOUT);
		}
#endif

		requested_mode = TIMESLOT_MODE_IDLE;
		return end_from_callback();
	}

	return none_from_callback();
}

static mpsl_timeslot_signal_return_param_t *timeslot_radio_action(void)
{
	handle_radio_end();

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	if (requested_mode == TIMESLOT_MODE_TX && atomic_get(&tx_done) != 0) {
		if (radio_txen_initialized) {
			nrfx_gppi_conn_disable(radio_txen_handle);
		}
		clear_txen_timer();
		radio_disable_and_wait();
		requested_mode = TIMESLOT_MODE_IDLE;
		clear_tx_timeout_timer();
		return end_from_callback();
	}
#endif

	return none_from_callback();
}

static mpsl_timeslot_signal_return_param_t *timeslot_reschedule_action(
	uint32_t signal)
{
	if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
		stats.timeslot_blocked++;
	} else if (signal == MPSL_TIMESLOT_SIGNAL_CANCELLED) {
		stats.timeslot_cancelled++;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	if (requested_mode == TIMESLOT_MODE_TX && atomic_get(&tx_done) == 0) {
		stats.busy_errors++;
		complete_tx_from_callback(-EBUSY);
		requested_mode = TIMESLOT_MODE_IDLE;
		return NULL;
	}
#endif

	if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
		(void)mpsl_timeslot_request(timeslot_session_id,
					    &timeslot_request_earliest);
	}

	return NULL;
}

static mpsl_timeslot_signal_return_param_t *timeslot_callback(
	mpsl_timeslot_session_id_t session_id, uint32_t signal)
{
	ARG_UNUSED(session_id);
	timeslot_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		return timeslot_start_action();
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		return timeslot_timer_action();
	case MPSL_TIMESLOT_SIGNAL_RADIO:
		return timeslot_radio_action();
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		return timeslot_reschedule_action(signal);
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
			(void)mpsl_timeslot_request(timeslot_session_id,
						    &timeslot_request_earliest);
		}
		return NULL;
	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
		stats.timeslot_overstayed++;
		return NULL;
	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
	case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
	default:
		return NULL;
	}
}

int radio_sync_init(uint8_t channel, uint32_t network_id)
{
	int ret;

	configured_channel = channel;
	configured_network_id = network_id;
	timeslot_requests_init();

	ret = radio_capture_init();
	if (ret != 0) {
		return ret;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	ret = radio_txen_trigger_init();
	if (ret != 0) {
		return ret;
	}
#endif

	ret = mpsl_timeslot_session_open(timeslot_callback,
					 &timeslot_session_id);
	if (ret != 0) {
		return ret;
	}

	timeslot_session_opened = true;
	return 0;
}

int radio_sync_send_beacon(const struct sync_beacon *beacon, uint64_t *tx_tick)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	uint64_t txen_tick = timebase_now_us() + RADIO_TX_DEFAULT_ARM_AHEAD_US;

	return radio_sync_send_beacon_at(beacon, txen_tick, 0u, tx_tick);
#else
	ARG_UNUSED(beacon);
	ARG_UNUSED(tx_tick);

	return -ENOTSUP;
#endif
}

int radio_sync_send_beacon_at(const struct sync_beacon *beacon,
			      uint64_t txen_tick, uint32_t txen_to_address_us,
			      uint64_t *address_tick)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	int ret;

	if (beacon == NULL) {
		return -EINVAL;
	}

	if (!timeslot_session_opened) {
		return -EACCES;
	}

	if (requested_mode != TIMESLOT_MODE_IDLE) {
		stats.busy_errors++;
		return -EBUSY;
	}

	if (txen_tick <= timebase_now_us() + RADIO_TX_ARM_AHEAD_US) {
		stats.busy_errors++;
		return -ETIME;
	}

	tx_packet[0] = SYNC_BEACON_WIRE_SIZE;
	pending_tx_beacon = *beacon;
	requested_address_tick = txen_tick + txen_to_address_us;
	pending_tx_beacon.master_tx_tick = requested_address_tick;
	ret = encode_tx_packet(requested_address_tick);
	if (ret != 0) {
		return ret;
	}

	requested_txen_tick = txen_tick;
	requested_txen_to_address_us = txen_to_address_us;
	atomic_set(&tx_done, 0);
	atomic_set(&tx_result, -EINPROGRESS);
	requested_mode = TIMESLOT_MODE_TX;

	ret = mpsl_timeslot_request(timeslot_session_id,
				    &timeslot_request_earliest);
	if (ret != 0) {
		requested_mode = TIMESLOT_MODE_IDLE;
		stats.busy_errors++;
		return ret;
	}

	uint64_t wait_start = timebase_now_us();
	uint64_t wait_timeout_us = TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE +
				   RADIO_TX_REQUEST_TIMEOUT_MARGIN_US;

	while (atomic_get(&tx_done) == 0 &&
	       timebase_now_us() - wait_start < wait_timeout_us) {
		k_sleep(K_USEC(50));
	}

	if (atomic_get(&tx_done) == 0) {
		requested_mode = TIMESLOT_MODE_IDLE;
		stats.busy_errors++;
		return -ETIMEDOUT;
	}

	requested_mode = TIMESLOT_MODE_IDLE;
	ret = atomic_get(&tx_result);
	if (ret != 0) {
		return ret;
	}

	if (address_tick != NULL) {
		*address_tick = requested_address_tick;
	}

	return 0;
#else
	ARG_UNUSED(beacon);
	ARG_UNUSED(txen_tick);
	ARG_UNUSED(txen_to_address_us);
	ARG_UNUSED(address_tick);

	return -ENOTSUP;
#endif
}

int radio_sync_start_rx(void)
{
	int ret;

	if (!timeslot_session_opened) {
		return -EACCES;
	}

	rx_enabled = true;
	requested_mode = TIMESLOT_MODE_RX;

	ret = mpsl_timeslot_request(timeslot_session_id,
				    &timeslot_request_earliest);
	if (ret != 0 && ret != -NRF_EAGAIN) {
		stats.busy_errors++;
		return ret;
	}

	return 0;
}

int radio_sync_poll_rx(struct radio_sync_rx *rx)
{
	unsigned int key;

	if (rx == NULL) {
		return -EINVAL;
	}

	if (!atomic_cas(&rx_state, 1, 2)) {
		return -EAGAIN;
	}

	key = irq_lock();
	*rx = pending_rx;
	irq_unlock(key);
	atomic_set(&rx_state, 0);

	return 0;
}

const struct radio_sync_stats *radio_sync_stats_get(void)
{
	return &stats;
}
