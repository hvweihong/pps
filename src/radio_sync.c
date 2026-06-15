#include "radio_sync.h"

#include "timebase.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hal/nrf_radio.h>
#include <helpers/nrfx_gppi.h>

#define RADIO_IRQ_PRIORITY 1
#define RADIO_PACKET_SIZE (SYNC_BEACON_WIRE_SIZE + 1u)
#define RADIO_ADDRESS_BASE 0x89abcdefu
#define RADIO_ADDRESS_PREFIX 0x45u
#define RADIO_DISABLED_TIMEOUT_US 1000u
#define RADIO_SCHEDULED_TX_TIMEOUT_MARGIN_US 2000u
#define RADIO_CAPTURE_CHANNEL 3u
#define RADIO_TX_ARM_AHEAD_US 160u

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static uint8_t tx_packet[RADIO_PACKET_SIZE] __aligned(4);
#endif
static uint8_t rx_packet[RADIO_PACKET_SIZE] __aligned(4);
static struct radio_sync_rx pending_rx;
static volatile bool rx_pending;
static uint32_t configured_network_id;
static struct radio_sync_stats stats;
#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
static nrfx_gppi_handle_t radio_capture_handle;
static bool radio_capture_initialized;
#endif
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static nrfx_gppi_handle_t radio_txen_handle;
static bool radio_txen_initialized;
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
static int radio_capture_init(void)
{
	uint32_t address_event =
		nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	uint32_t capture_task =
		timebase_timer_capture_task_address(RADIO_CAPTURE_CHANNEL);
	int ret;

	ret = nrfx_gppi_conn_alloc(address_event, capture_task,
				   &radio_capture_handle);
	if (ret != 0) {
		return ret;
	}

	nrfx_gppi_conn_enable(radio_capture_handle);
	radio_capture_initialized = true;

	return 0;
}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static int radio_txen_trigger_init(void)
{
	uint32_t compare_event =
		timebase_timer_event_address(RADIO_CAPTURE_CHANNEL);
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

static void radio_configure_common(uint8_t channel)
{
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

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
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK);
}

static int wait_disabled_timeout(uint64_t timeout_us)
{
	uint64_t start = timebase_now_us();

	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		if (timebase_now_us() - start > timeout_us) {
			return -ETIMEDOUT;
		}
	}

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	return 0;
}

static int wait_disabled(void)
{
	return wait_disabled_timeout(RADIO_DISABLED_TIMEOUT_US);
}

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static void radio_set_tx_shorts(void)
{
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK);
}
#endif

static void radio_set_rx_shorts(void)
{
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_DISABLE_MASK |
			     NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
		return;
	}

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	uint64_t timestamp = radio_capture_initialized ?
		timebase_capture64(RADIO_CAPTURE_CHANNEL) : timebase_now_us();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
#else
	uint64_t timestamp = timebase_now_us();
#endif

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

	if (rx_packet[0] == SYNC_BEACON_WIRE_SIZE &&
	    nrf_radio_crc_status_check(NRF_RADIO)) {
		struct sync_beacon beacon;
		int ret = sync_beacon_decode(&rx_packet[1], SYNC_BEACON_WIRE_SIZE,
					     configured_network_id, &beacon);

		if (ret == 0) {
			pending_rx.beacon = beacon;
			pending_rx.local_rx_tick = timestamp;
			rx_pending = true;
			stats.rx_packets++;
		} else {
			stats.rx_decode_errors++;
		}
	} else if (rx_packet[0] == SYNC_BEACON_WIRE_SIZE) {
		stats.rx_crc_errors++;
	}
}

int radio_sync_init(uint8_t channel, uint32_t network_id)
{
	configured_network_id = network_id;
	radio_configure_common(channel);

	int ret;

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	ret = radio_capture_init();
	if (ret != 0) {
		return ret;
	}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	ret = radio_txen_trigger_init();
	if (ret != 0) {
		return ret;
	}
#endif

	IRQ_CONNECT(RADIO_IRQn, RADIO_IRQ_PRIORITY, radio_isr, NULL, 0);
	irq_enable(RADIO_IRQn);

	return 0;
}

int radio_sync_send_beacon(const struct sync_beacon *beacon, uint64_t *tx_tick)
{
#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	uint64_t txen_tick = timebase_now_us() + (2u * RADIO_TX_ARM_AHEAD_US);

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

	if (!radio_txen_initialized) {
		return -EACCES;
	}

	if (txen_tick <= timebase_now_us() + RADIO_TX_ARM_AHEAD_US) {
		return -ETIME;
	}

	if (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED) {
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		ret = wait_disabled();
		if (ret != 0) {
			stats.busy_errors++;
			return ret;
		}
	}

	radio_set_tx_shorts();
	tx_packet[0] = SYNC_BEACON_WIRE_SIZE;
	struct sync_beacon wire_beacon = *beacon;
	uint64_t stamped_tick = txen_tick + txen_to_address_us;

	wire_beacon.master_tx_tick = stamped_tick;
	ret = sync_beacon_encode(&wire_beacon, &tx_packet[1],
				 SYNC_BEACON_WIRE_SIZE);
	if (ret != 0) {
		return ret;
	}

	nrf_radio_packetptr_set(NRF_RADIO, tx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	ret = timebase_set_compare_no_irq(RADIO_CAPTURE_CHANNEL,
					  (uint32_t)txen_tick);
	if (ret != 0) {
		return ret;
	}

	uint64_t now = timebase_now_us();

	if (txen_tick <= now + RADIO_TX_ARM_AHEAD_US) {
		return -ETIME;
	}

	nrfx_gppi_conn_enable(radio_txen_handle);
	ret = wait_disabled_timeout((txen_tick - now) +
				    RADIO_SCHEDULED_TX_TIMEOUT_MARGIN_US);
	nrfx_gppi_conn_disable(radio_txen_handle);
	if (ret != 0) {
		stats.busy_errors++;
		return ret;
	}

	if (address_tick != NULL) {
		*address_tick = stamped_tick;
	}

	stats.tx_packets++;
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
	if (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED) {
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		(void)wait_disabled();
	}

	rx_packet[0] = 0;
	memset(&rx_packet[1], 0, SYNC_BEACON_WIRE_SIZE);
	radio_set_rx_shorts();
	nrf_radio_packetptr_set(NRF_RADIO, rx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

	return 0;
}

int radio_sync_poll_rx(struct radio_sync_rx *rx)
{
	unsigned int key;

	if (rx == NULL) {
		return -EINVAL;
	}

	key = irq_lock();
	if (!rx_pending) {
		irq_unlock(key);
		return -EAGAIN;
	}

	*rx = pending_rx;
	rx_pending = false;
	irq_unlock(key);

	return 0;
}

const struct radio_sync_stats *radio_sync_stats_get(void)
{
	return &stats;
}
