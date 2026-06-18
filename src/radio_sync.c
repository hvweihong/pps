#include "radio_sync.h"

#include "app_config.h"
#include "time_sync_math.h"
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
#define RADIO_RX_EARLIEST_MAX_WAIT_US 1000u
#define RADIO_RX_SLOT_GUARD_US 80u
#define RADIO_TIMESLOT_MIN_LENGTH_US 100u
#define RADIO_DISABLE_WAIT_US 100u
#define RADIO_BASE_FREQUENCY_MHZ 2400u
#define RADIO_ADDRESS_INDEX 0u
#define RADIO_RX_ADDRESS_MASK (1u << RADIO_ADDRESS_INDEX)
#define RADIO_IRQ_MASK \
	(NRF_RADIO_INT_READY_MASK | NRF_RADIO_INT_ADDRESS_MASK | NRF_RADIO_INT_END_MASK)
#define TIMESLOT_END_TIMER_CHANNEL NRF_TIMER_CC_CHANNEL0
#define TIMESLOT_TX_TIMEOUT_CHANNEL NRF_TIMER_CC_CHANNEL1
#define TIMESLOT_CAPTURE_CHANNEL NRF_TIMER_CC_CHANNEL2
#define TIMESLOT_TXEN_TIMER_CHANNEL NRF_TIMER_CC_CHANNEL3
#define TIMESLOT_RXEN_TIMER_CHANNEL NRF_TIMER_CC_CHANNEL1

enum timeslot_mode {
	TIMESLOT_MODE_IDLE,
	TIMESLOT_MODE_RX,
	TIMESLOT_MODE_TX,
	TIMESLOT_MASTER_TX_ACTIVE,
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
static uint32_t active_timeslot_length_us;
static bool timeslot_anchor_valid;
static uint64_t timeslot_anchor_tick;
static uint64_t requested_rx_start_tick;
static uint32_t requested_rx_window_length_us;
static uint32_t requested_rx_timeslot_length_us;
static bool rx_packet_received;
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static atomic_t tx_done;
static atomic_t tx_result;
static uint64_t requested_txen_tick;
static uint64_t requested_address_tick;
static uint32_t requested_txen_to_address_us;
static struct radio_sync_master_config master_tx_cfg;
static uint64_t master_tx_pps_epoch_tick;
static bool master_tx_active;
#endif

static uint16_t radio_frequency_mhz(uint8_t channel)
{
	return (uint16_t)(RADIO_BASE_FREQUENCY_MHZ + channel);
}

static uint32_t timeslot_length_us(void)
{
	if (TIME_SYNC_TIMESLOT_LENGTH_US_VALUE < RADIO_TIMESLOT_MIN_LENGTH_US) {
		return RADIO_TIMESLOT_MIN_LENGTH_US;
	}

	return TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static uint32_t master_tx_timeslot_length_us(void)
{
	uint32_t length = TIME_SYNC_MASTER_TX_TIMESLOT_LENGTH_US_VALUE;

	if (length < RADIO_TIMESLOT_MIN_LENGTH_US) {
		return RADIO_TIMESLOT_MIN_LENGTH_US;
	}

	if (length > TIME_SYNC_TIMESLOT_LENGTH_US_VALUE) {
		return TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
	}

	return length;
}
#endif

static uint32_t timeslot_end_margin_us(void)
{
	uint32_t length = active_timeslot_length_us != 0u ?
		active_timeslot_length_us : timeslot_length_us();
	uint32_t margin = TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE;

	if (margin >= length) {
		return length / 2u;
	}

	return margin;
}

static uint32_t timeslot_cleanup_at_us(void)
{
	uint32_t length = active_timeslot_length_us != 0u ?
		active_timeslot_length_us : timeslot_length_us();
	uint32_t margin = timeslot_end_margin_us();

	if (margin >= length) {
		return length / 2u;
	}

	return length - margin;
}

static uint32_t rx_start_offset_us(void)
{
	uint32_t offset = TIME_SYNC_RX_START_OFFSET_US_VALUE;

	if (offset < RADIO_TX_ARM_AHEAD_US) {
		return RADIO_TX_ARM_AHEAD_US;
	}

	return offset;
}

static uint32_t rx_timeslot_length_us(uint32_t window_len)
{
	uint32_t length = rx_start_offset_us() + window_len +
			  TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE +
			  TIME_SYNC_RX_WINDOW_GUARD_US_VALUE;

	if (length < RADIO_TIMESLOT_MIN_LENGTH_US) {
		return RADIO_TIMESLOT_MIN_LENGTH_US;
	}

	if (length > TIME_SYNC_TIMESLOT_LENGTH_US_VALUE) {
		return TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
	}

	return length;
}

static bool mode_is_tx(enum timeslot_mode mode)
{
	return mode == TIMESLOT_MODE_TX ||
	       mode == TIMESLOT_MASTER_TX_ACTIVE;
}

static bool rx_window_fits_timeslot(uint32_t window_len, uint32_t timeslot_len)
{
	return rx_start_offset_us() + window_len +
		       TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE +
		       TIME_SYNC_RX_WINDOW_GUARD_US_VALUE <= timeslot_len;
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
	uint16_t freq_mhz = radio_frequency_mhz(channel);

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_frequency_set(NRF_RADIO, freq_mhz);
	stats.radio_frequency_mhz = nrf_radio_frequency_get(NRF_RADIO);
	nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
	nrf_radio_base0_set(NRF_RADIO, RADIO_ADDRESS_BASE);
	nrf_radio_prefix0_set(NRF_RADIO, RADIO_ADDRESS_PREFIX);
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_rxaddresses_set(NRF_RADIO, RADIO_RX_ADDRESS_MASK);

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
	nrf_radio_datawhiteiv_set(NRF_RADIO, channel);
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	stats.tx_address = nrf_radio_txaddress_get(NRF_RADIO);
	stats.rx_address_mask = nrf_radio_rxaddresses_get(NRF_RADIO);
	stats.radio_datawhiteiv = nrf_radio_datawhiteiv_get(NRF_RADIO);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
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
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL));
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_START);
	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	active_timeslot_ref_count =
		nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);
	active_timeslot_ref_tick = timebase_now_us();
	timeslot_anchor_tick = active_timeslot_ref_tick;
	timeslot_anchor_valid = true;
}

static void clear_slot_end_timer(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
}

static void clear_rx_timer(void)
{
	nrf_timer_int_disable(
		MPSL_TIMER0,
		nrf_timer_compare_int_get(TIMESLOT_RXEN_TIMER_CHANNEL));
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL));
}

static void clear_compare1_timer(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static void clear_tx_timeout_timer(void)
{
	clear_compare1_timer();
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

static mpsl_timeslot_request_t *select_rx_timeslot_request(
	uint64_t start_tick, uint32_t timeslot_len,
	enum radio_sync_rx_mode mode)
{
	uint64_t now = timebase_now_us();
	uint64_t distance;

	timeslot_request_earliest.params.earliest.length_us = timeslot_len;
	timeslot_request_normal.params.normal.length_us = timeslot_len;

	if (mode == RADIO_SYNC_RX_ACQUIRE) {
		timeslot_request_earliest.params.earliest.timeout_us =
			TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE;
		return &timeslot_request_earliest;
	}

	if (timeslot_anchor_valid && start_tick > timeslot_anchor_tick) {
		distance = start_tick - timeslot_anchor_tick;
		if (distance <= MPSL_TIMESLOT_DISTANCE_MAX_US) {
			timeslot_request_normal.params.normal.distance_us =
				(uint32_t)distance;
			return &timeslot_request_normal;
		}
	}

	if (start_tick <= now + RADIO_RX_EARLIEST_MAX_WAIT_US) {
		timeslot_request_earliest.params.earliest.timeout_us =
			RADIO_RX_EARLIEST_MAX_WAIT_US;
		return &timeslot_request_earliest;
	}

	return NULL;
}

static int arm_rx_window_end_timer(uint64_t rxen_slot_tick);

static void trigger_rxen_now(void)
{
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	stats.rxen_events++;
	stats.last_rx_radio_state = nrf_radio_state_get(NRF_RADIO);

	if (arm_rx_window_end_timer(rx_start_offset_us()) != 0) {
		stats.rx_window_late++;
	}
}

static int arm_rx_window_end_timer(uint64_t rxen_slot_tick)
{
	uint64_t rx_end = rxen_slot_tick + requested_rx_window_length_us;

	if (rx_end + TIME_SYNC_RX_WINDOW_GUARD_US_VALUE >
	    timeslot_cleanup_at_us()) {
		stats.rx_window_late++;
		return -ETIME;
	}

	nrf_timer_cc_set(MPSL_TIMER0, TIMESLOT_TX_TIMEOUT_CHANNEL,
			 (uint32_t)rx_end);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
	nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);

	return 0;
}

static int prepare_rx_in_timeslot(void)
{
	uint64_t now = timebase_now_us();
	uint32_t slot_now;
	uint64_t slot_rxen;
	uint32_t rx_offset = rx_start_offset_us();

	rx_packet_received = false;
	rx_packet[0] = 0;
	memset(&rx_packet[1], 0, SYNC_BEACON_WIRE_SIZE);
	radio_configure_common(configured_channel);
	stats.last_rx_radio_state = nrf_radio_state_get(NRF_RADIO);
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK |
			     NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
	nrf_radio_packetptr_set(NRF_RADIO, rx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	if (radio_capture_initialized) {
		nrfx_gppi_conn_enable(radio_capture_handle);
	}

	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	slot_now = nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);

	if ((stats.rx_mode != RADIO_SYNC_RX_ACQUIRE &&
	     requested_rx_start_tick <= now + RADIO_TX_ARM_AHEAD_US) ||
	    slot_now + RADIO_TX_ARM_AHEAD_US >= rx_offset) {
		stats.rx_window_late++;
		return -ETIME;
	}

	slot_rxen = rx_offset;

	if (slot_rxen + RADIO_TX_SLOT_GUARD_US >= timeslot_cleanup_at_us()) {
		stats.rx_window_late++;
		return -ETIME;
	}

	nrf_timer_cc_set(MPSL_TIMER0, TIMESLOT_RXEN_TIMER_CHANNEL,
			 (uint32_t)slot_rxen);
	nrf_timer_event_clear(
		MPSL_TIMER0,
		nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL));
	nrf_timer_int_enable(
		MPSL_TIMER0,
		nrf_timer_compare_int_get(TIMESLOT_RXEN_TIMER_CHANNEL));

	return 0;
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static int encode_tx_packet(uint64_t master_tx_tick)
{
	struct sync_beacon wire_beacon = pending_tx_beacon;

	wire_beacon.master_tx_tick = master_tx_tick;
	tx_packet[0] = SYNC_BEACON_WIRE_SIZE;
	stats.last_tx_packet_len = tx_packet[0];
	return sync_beacon_encode(&wire_beacon, &tx_packet[1],
				  SYNC_BEACON_WIRE_SIZE);
}

static void prepare_master_tx_beacon(uint64_t address_tick)
{
	uint64_t pps_tick;

	pps_tick = time_sync_next_epoch_tick_after(master_tx_pps_epoch_tick,
						   master_tx_cfg.pps_period_us,
						   address_tick);

	pending_tx_beacon = (struct sync_beacon) {
		.magic = SYNC_BEACON_MAGIC,
		.version = SYNC_BEACON_VERSION,
		.role = SYNC_BEACON_ROLE_MASTER,
		.seq = (uint16_t)stats.tx_sequence,
		.network_id = master_tx_cfg.network_id,
		.master_tx_tick = address_tick,
		.next_pps_master_tick = pps_tick,
		.sync_interval_us = master_tx_cfg.sync_interval_us,
		.status_flags = 0,
	};
}

static int prepare_tx_in_timeslot(void)
{
	uint64_t now = timebase_now_us();
	uint64_t txen_tick = requested_txen_tick;
	uint32_t slot_now;
	uint64_t slot_txen;
	uint32_t tx_timeout_us;

	if (requested_mode == TIMESLOT_MASTER_TX_ACTIVE) {
		txen_tick = now + TIME_SYNC_MASTER_TX_START_OFFSET_US_VALUE;
		requested_txen_tick = txen_tick;
		requested_txen_to_address_us = master_tx_cfg.txen_to_address_us;
	}

	if (!radio_txen_initialized ||
	    txen_tick <= now + RADIO_TX_ARM_AHEAD_US) {
		stats.tx_timing_errors++;
		stats.tx_prepare_late++;
		return -ETIME;
	}

	nrf_timer_task_trigger(MPSL_TIMER0,
			       nrf_timer_capture_task_get(TIMESLOT_CAPTURE_CHANNEL));
	slot_now = nrf_timer_cc_get(MPSL_TIMER0, TIMESLOT_CAPTURE_CHANNEL);
	slot_txen = (uint64_t)slot_now + (txen_tick - now);

	if (slot_txen + RADIO_TX_COMPLETE_TIMEOUT_US + RADIO_TX_SLOT_GUARD_US >=
	    timeslot_cleanup_at_us()) {
		stats.tx_timing_errors++;
		stats.tx_prepare_late++;
		return -ETIME;
	}

	radio_configure_common(configured_channel);
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK);
	nrf_radio_packetptr_set(NRF_RADIO, tx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	if (radio_capture_initialized) {
		nrfx_gppi_conn_enable(radio_capture_handle);
	}

	requested_address_tick = txen_tick + requested_txen_to_address_us;
	if (requested_mode == TIMESLOT_MASTER_TX_ACTIVE) {
		prepare_master_tx_beacon(requested_address_tick);
	}
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

	if (mode_is_tx(requested_mode)) {
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (radio_txen_initialized) {
			nrfx_gppi_conn_disable(radio_txen_handle);
		}
		clear_txen_timer();
		if (radio_capture_initialized) {
			nrfx_gppi_conn_disable(radio_capture_handle);
		}
		stats.tx_packets++;
		if (requested_mode == TIMESLOT_MASTER_TX_ACTIVE) {
			stats.tx_sequence++;
		}
		stats.last_tx_error_us =
			(int32_t)((int64_t)timestamp -
				  (int64_t)requested_address_tick);
		stats.last_tx_ref_age_us = ref_age_us;
		complete_tx_from_callback(0);
#endif
		radio_disable_now();
		return;
	}

	stats.rx_end_events++;
	stats.last_rx_packet_len = rx_packet[0];
	stats.last_rx_crc_ok = nrf_radio_crc_status_check(NRF_RADIO);

	if (rx_packet[0] == SYNC_BEACON_WIRE_SIZE &&
	    stats.last_rx_crc_ok) {
		struct sync_beacon beacon;
		int ret = sync_beacon_decode(&rx_packet[1], SYNC_BEACON_WIRE_SIZE,
					     configured_network_id, &beacon);

		if (ret == 0) {
			if (atomic_cas(&rx_state, 0, 2)) {
				pending_rx.beacon = beacon;
				pending_rx.local_rx_tick = timestamp;
				atomic_set(&rx_state, 1);
				rx_packet_received = true;
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
	} else {
		stats.rx_bad_length_events++;
	}
}

static void record_rx_address_if_seen(void)
{
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_READY)) {
		if (requested_mode == TIMESLOT_MODE_RX) {
			stats.rx_ready_events++;
			stats.last_rx_radio_state = nrf_radio_state_get(NRF_RADIO);
		}
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		if (requested_mode == TIMESLOT_MODE_RX) {
			stats.rx_address_events++;
			stats.last_rx_radio_state = nrf_radio_state_get(NRF_RADIO);
		}
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	}
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

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static mpsl_timeslot_signal_return_param_t *request_master_tx_from_callback(void)
{
	active_timeslot_length_us = 0u;
	clear_txen_timer();
	clear_tx_timeout_timer();
	if (radio_txen_initialized) {
		nrfx_gppi_conn_disable(radio_txen_handle);
	}
	if (radio_capture_initialized) {
		nrfx_gppi_conn_disable(radio_capture_handle);
	}
	atomic_set(&tx_done, 0);
	atomic_set(&tx_result, -EINPROGRESS);
	requested_mode = TIMESLOT_MASTER_TX_ACTIVE;
	timeslot_request_normal.params.normal.distance_us =
		master_tx_cfg.sync_interval_us;
	timeslot_request_normal.params.normal.length_us =
		master_tx_timeslot_length_us();
	timeslot_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
	timeslot_return.params.request.p_next = &timeslot_request_normal;

	return &timeslot_return;
}

static int request_master_tx_earliest(void)
{
	int ret;

	atomic_set(&tx_done, 0);
	atomic_set(&tx_result, -EINPROGRESS);
	requested_mode = TIMESLOT_MASTER_TX_ACTIVE;
	timeslot_request_earliest.params.earliest.length_us =
		master_tx_timeslot_length_us();
	timeslot_request_earliest.params.earliest.timeout_us =
		TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE;

	ret = mpsl_timeslot_request(timeslot_session_id,
				    &timeslot_request_earliest);
	if (ret != 0) {
		requested_mode = TIMESLOT_MODE_IDLE;
		master_tx_active = false;
		stats.tx_mpsl_request_errors++;
		stats.busy_errors++;
	}

	return ret;
}
#endif

static void finish_rx_window_from_callback(void)
{
	clear_rx_timer();
	clear_compare1_timer();
	handle_radio_end();
	radio_disable_and_wait();

	if (radio_capture_initialized) {
		nrfx_gppi_conn_disable(radio_capture_handle);
	}

	requested_mode = TIMESLOT_MODE_IDLE;
	stats.rx_windows++;
	stats.rx_request_active = false;
	active_timeslot_length_us = 0u;
	requested_rx_window_length_us = 0u;
	requested_rx_timeslot_length_us = 0u;
	rx_packet_received = false;
}

static mpsl_timeslot_signal_return_param_t *timeslot_start_action(void)
{
	if (requested_mode == TIMESLOT_MODE_RX) {
		active_timeslot_length_us = requested_rx_timeslot_length_us;
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	} else if (requested_mode == TIMESLOT_MASTER_TX_ACTIVE) {
		active_timeslot_length_us = master_tx_timeslot_length_us();
#endif
	} else {
		active_timeslot_length_us = timeslot_length_us();
	}
	timeslot_timer_configure();
	arm_slot_end_timer();

	if (mode_is_tx(requested_mode)) {
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		int ret = prepare_tx_in_timeslot();

		if (ret == 0) {
			return none_from_callback();
		}

		complete_tx_from_callback(ret);
		radio_disable_and_wait();
		if (master_tx_active) {
			return request_master_tx_from_callback();
		}
		return end_from_callback();
#else
		return end_from_callback();
#endif
	}

	if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
		int ret = prepare_rx_in_timeslot();

		if (ret == 0) {
			return none_from_callback();
		}

		requested_mode = TIMESLOT_MODE_IDLE;
		stats.rx_request_active = false;
		clear_rx_timer();
		clear_compare1_timer();
		if (radio_capture_initialized) {
			nrfx_gppi_conn_disable(radio_capture_handle);
		}
		radio_disable_and_wait();
		return end_from_callback();
	}

	return end_from_callback();
}

static mpsl_timeslot_signal_return_param_t *timeslot_timer_action(void)
{
	if (requested_mode == TIMESLOT_MODE_RX &&
	    nrf_timer_event_check(
		    MPSL_TIMER0,
		    nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL))) {
		nrf_timer_int_disable(
			MPSL_TIMER0,
			nrf_timer_compare_int_get(TIMESLOT_RXEN_TIMER_CHANNEL));
		nrf_timer_event_clear(
			MPSL_TIMER0,
			nrf_timer_compare_event_get(TIMESLOT_RXEN_TIMER_CHANNEL));

		if (rx_enabled) {
			trigger_rxen_now();
		}
	}

	if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1)) {
		nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);

		if (requested_mode == TIMESLOT_MODE_RX) {
			finish_rx_window_from_callback();
			return end_from_callback();
		}

		handle_radio_end();

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (mode_is_tx(requested_mode) && atomic_get(&tx_done) != 0) {
			if (!master_tx_active) {
				requested_mode = TIMESLOT_MODE_IDLE;
			}
			clear_txen_timer();
			radio_disable_and_wait();
			if (master_tx_active) {
				return request_master_tx_from_callback();
			}
			return end_from_callback();
		}

		if (mode_is_tx(requested_mode) && atomic_get(&tx_done) == 0) {
			stats.busy_errors++;
			stats.tx_timeout_errors++;
			complete_tx_from_callback(-ETIMEDOUT);
			if (radio_txen_initialized) {
				nrfx_gppi_conn_disable(radio_txen_handle);
			}
			clear_txen_timer();
			radio_disable_and_wait();
			if (master_tx_active) {
				return request_master_tx_from_callback();
			}
			return end_from_callback();
		}
#endif
	}

	if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0)) {
		clear_slot_end_timer();

		if (requested_mode == TIMESLOT_MODE_RX) {
			finish_rx_window_from_callback();
			return end_from_callback();
		}

		clear_rx_timer();
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

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (mode_is_tx(requested_mode) && atomic_get(&tx_done) == 0) {
			stats.busy_errors++;
			stats.tx_timeout_errors++;
			complete_tx_from_callback(-ETIMEDOUT);
		}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
		if (master_tx_active) {
			return request_master_tx_from_callback();
		}
#endif
		requested_mode = TIMESLOT_MODE_IDLE;
		return end_from_callback();
	}

	return none_from_callback();
}

static mpsl_timeslot_signal_return_param_t *timeslot_radio_action(void)
{
	record_rx_address_if_seen();
	handle_radio_end();

	if (requested_mode == TIMESLOT_MODE_RX && rx_packet_received) {
		finish_rx_window_from_callback();
		return end_from_callback();
	}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	if (mode_is_tx(requested_mode) && atomic_get(&tx_done) != 0) {
		if (radio_txen_initialized) {
			nrfx_gppi_conn_disable(radio_txen_handle);
		}
		clear_txen_timer();
		radio_disable_and_wait();
		clear_tx_timeout_timer();
		if (master_tx_active) {
			return request_master_tx_from_callback();
		}
		requested_mode = TIMESLOT_MODE_IDLE;
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
	if (mode_is_tx(requested_mode) && atomic_get(&tx_done) == 0) {
		stats.busy_errors++;
		if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			stats.tx_mpsl_blocked++;
			complete_tx_from_callback(-EAGAIN);
		} else {
			stats.tx_mpsl_cancelled++;
			complete_tx_from_callback(-ECANCELED);
		}
		if (!master_tx_active) {
			requested_mode = TIMESLOT_MODE_IDLE;
		} else {
			(void)request_master_tx_earliest();
		}
		return NULL;
	}
#endif

	if (requested_mode == TIMESLOT_MODE_RX && rx_enabled) {
		stats.rx_window_skips++;
		requested_mode = TIMESLOT_MODE_IDLE;
		stats.rx_request_active = false;
		active_timeslot_length_us = 0u;
		requested_rx_window_length_us = 0u;
		requested_rx_timeslot_length_us = 0u;
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
			requested_mode = TIMESLOT_MODE_IDLE;
			stats.rx_request_active = false;
			active_timeslot_length_us = 0u;
			requested_rx_window_length_us = 0u;
			requested_rx_timeslot_length_us = 0u;
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

int radio_sync_reconfigure(uint8_t channel, uint32_t network_id)
{
	configured_channel = channel;
	configured_network_id = network_id;
	timeslot_anchor_valid = false;
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
		stats.tx_api_busy_errors++;
		return -EBUSY;
	}

	if (txen_tick <= timebase_now_us() + RADIO_TX_ARM_AHEAD_US) {
		stats.busy_errors++;
		stats.tx_prepare_late++;
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
	timeslot_request_earliest.params.earliest.length_us = timeslot_length_us();
	timeslot_request_earliest.params.earliest.timeout_us =
		TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US_VALUE;

	ret = mpsl_timeslot_request(timeslot_session_id,
				    &timeslot_request_earliest);
	if (ret != 0) {
		requested_mode = TIMESLOT_MODE_IDLE;
		stats.busy_errors++;
		stats.tx_mpsl_request_errors++;
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
		stats.tx_timeout_errors++;
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

int radio_sync_start_master(const struct radio_sync_master_config *cfg,
			    uint64_t first_txen_tick)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	if (cfg == NULL ||
	    cfg->network_id == 0u ||
	    cfg->sync_interval_us == 0u ||
	    cfg->pps_period_us == 0u ||
	    cfg->pps_epoch_tick == 0u) {
		return -EINVAL;
	}

	if (!timeslot_session_opened) {
		return -EACCES;
	}

	if (master_tx_active || requested_mode != TIMESLOT_MODE_IDLE) {
		stats.busy_errors++;
		stats.tx_api_busy_errors++;
		return -EBUSY;
	}

	if (first_txen_tick <= timebase_now_us() + RADIO_TX_ARM_AHEAD_US) {
		stats.tx_prepare_late++;
		return -ETIME;
	}

	master_tx_cfg = *cfg;
	stats.tx_sequence = cfg->initial_seq;
	requested_txen_tick = first_txen_tick;
	requested_txen_to_address_us = cfg->txen_to_address_us;
	master_tx_pps_epoch_tick = cfg->pps_epoch_tick;
	master_tx_active = true;

	return request_master_tx_earliest();
#else
	ARG_UNUSED(cfg);
	ARG_UNUSED(first_txen_tick);

	return -ENOTSUP;
#endif
}

int radio_sync_stop_master(void)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	master_tx_active = false;
	if (requested_mode == TIMESLOT_MASTER_TX_ACTIVE) {
		requested_mode = TIMESLOT_MODE_IDLE;
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

int radio_sync_start_rx(void)
{
	struct radio_sync_window window = {
		.start_tick = timebase_now_us() + 2000u,
		.length_us = TIME_SYNC_RX_ACQUIRE_WINDOW_US_VALUE,
		.mode = RADIO_SYNC_RX_ACQUIRE,
	};

	return radio_sync_schedule_rx_window(&window);
}

int radio_sync_stop_rx(void)
{
	rx_enabled = false;
	if (requested_mode == TIMESLOT_MODE_RX) {
		requested_mode = TIMESLOT_MODE_IDLE;
	}
	requested_rx_start_tick = 0;
	requested_rx_window_length_us = 0;
	requested_rx_timeslot_length_us = 0;
	rx_packet_received = false;
	stats.rx_request_active = false;
	stats.rx_mode = RADIO_SYNC_RX_STOPPED;
	return 0;
}

int radio_sync_schedule_rx_window(const struct radio_sync_window *window)
{
	mpsl_timeslot_request_t *request;
	uint32_t timeslot_len;
	int ret;
	uint64_t now = timebase_now_us();
	uint64_t timeslot_start_tick;
	uint32_t rx_offset = rx_start_offset_us();

	if (window == NULL ||
	    window->length_us < RADIO_TIMESLOT_MIN_LENGTH_US ||
	    window->length_us > MPSL_TIMESLOT_LENGTH_MAX_US) {
		return -EINVAL;
	}

	if (!timeslot_session_opened) {
		return -EACCES;
	}

	if (requested_mode != TIMESLOT_MODE_IDLE) {
		stats.busy_errors++;
		return -EBUSY;
	}

	if (window->mode != RADIO_SYNC_RX_ACQUIRE &&
	    (window->start_tick <= rx_offset ||
	     window->start_tick - rx_offset <= now + RADIO_TX_ARM_AHEAD_US)) {
		stats.rx_window_late++;
		return -ETIME;
	}

	timeslot_len = rx_timeslot_length_us(window->length_us);
	if (!rx_window_fits_timeslot(window->length_us, timeslot_len)) {
		stats.rx_window_late++;
		return -EINVAL;
	}

	timeslot_start_tick = window->start_tick - rx_offset;
	request = select_rx_timeslot_request(timeslot_start_tick, timeslot_len,
					     window->mode);
	if (request == NULL) {
		return -EAGAIN;
	}

	requested_rx_window_length_us = window->length_us;
	requested_rx_timeslot_length_us = timeslot_len;
	requested_rx_start_tick = window->start_tick;
	rx_packet_received = false;
	stats.rx_window_length_us = window->length_us;
	stats.rx_mode = window->mode;
	stats.rx_request_active = true;
	rx_enabled = true;
	requested_mode = TIMESLOT_MODE_RX;

	ret = mpsl_timeslot_request(timeslot_session_id, request);
	if (ret != 0) {
		requested_mode = TIMESLOT_MODE_IDLE;
		rx_packet_received = false;
		stats.rx_request_active = false;
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
