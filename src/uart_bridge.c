#include "uart_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "byte_ring.h"

#if !DT_NODE_HAS_STATUS(DT_ALIAS(bridge_uart), okay)
#error "bridge-uart alias is required"
#endif

#define RB_UART_DEVICE DT_ALIAS(bridge_uart)
#define RB_UART_RX_BUFFER_SIZE 512u
#define RB_UART_TX_BUFFER_SIZE 512u

static const struct device *const uart_dev = DEVICE_DT_GET(RB_UART_DEVICE);
static uint8_t rx_dma_buf[2][RB_UART_RX_BUFFER_SIZE];
static uint8_t tx_dma_buf[RB_UART_TX_BUFFER_SIZE];
static uint8_t rx_storage[CONFIG_RADIO_BRIDGE_UART_RING_SIZE];
static uint8_t tx_storage[CONFIG_RADIO_BRIDGE_UART_RING_SIZE];
static struct rb_byte_ring rx_ring;
static struct rb_byte_ring tx_ring;
static struct rb_uart_stats stats;
static struct rb_uart_stats stats_snapshot;
static struct k_spinlock state_lock;
static bool tx_busy;
static size_t tx_inflight_len;
static size_t tx_dma_offset;
static bool tx_retry_active;
static uint8_t next_rx_buffer;
static rb_uart_bridge_wake_fn wake_callback;

/*
 * The async UART callback is allowed to run from interrupt context.  Keep all
 * ring and transfer state behind state_lock and call this helper only while
 * the lock is held.  The nRF UARTE uart_tx() operation is non-blocking and
 * irq-safe; holding the lock prevents a same-CPU callback from observing the
 * half-published transfer state.
 */
static void start_pending_tx_locked(void)
{
	size_t n;
	int err;

	if (tx_busy) {
		return;
	}
	if (tx_retry_active) {
		n = tx_inflight_len;
		err = uart_tx(uart_dev, tx_dma_buf + tx_dma_offset, n, 0);
	} else {
		n = rb_byte_ring_peek(&tx_ring, tx_dma_buf, sizeof(tx_dma_buf));
		if (n == 0u) {
			return;
		}
		/* uart_tx() is non-blocking; remove only bytes handed to EasyDMA. */
		err = uart_tx(uart_dev, tx_dma_buf, n, 0);
		if (err == 0) {
			(void)rb_byte_ring_discard(&tx_ring, n);
		}
	}
	if (err == 0) {
		tx_busy = true;
		tx_inflight_len = n;
	} else if (!tx_retry_active) {
		tx_inflight_len = 0u;
	}
	if (err != 0 && tx_retry_active) {
		/* Keep retry bytes for a later uart_bridge_write() attempt. */
		tx_busy = false;
	}
}

static void capture_rx_locked(const uint8_t *data, size_t len)
{
	size_t accepted;

	if (data == NULL || len == 0u) {
		return;
	}
	accepted = rb_byte_ring_write(&rx_ring, data, len);
	stats.rx_bytes += accepted;
	stats.rx_drop_bytes = rx_ring.dropped_bytes;
}

static void uart_callback(const struct device *dev, struct uart_event *event,
			  void *user_data)
{
	(void)dev;
	(void)user_data;

	switch (event->type) {
	case UART_RX_RDY:
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		capture_rx_locked(event->data.rx.buf + event->data.rx.offset,
				  event->data.rx.len);
		k_spin_unlock(&state_lock, key);
		if (wake_callback != NULL) {
			wake_callback();
		}
	}
		break;
	case UART_RX_STOPPED:
	{
		const struct uart_event_rx *rx = &event->data.rx_stop.data;
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		capture_rx_locked(rx->buf + rx->offset, rx->len);
		k_spin_unlock(&state_lock, key);
		if (wake_callback != NULL) {
			wake_callback();
		}
	}
		break;
	case UART_RX_BUF_REQUEST:
		if (uart_rx_buf_rsp(uart_dev, rx_dma_buf[next_rx_buffer],
				    sizeof(rx_dma_buf[next_rx_buffer])) == 0) {
			next_rx_buffer ^= 1u;
		}
		break;
	case UART_RX_BUF_RELEASED:
		break;
	case UART_RX_DISABLED:
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		stats.rx_disabled_events++;
		k_spin_unlock(&state_lock, key);
	}
		if (uart_rx_enable(uart_dev, rx_dma_buf[next_rx_buffer],
				   sizeof(rx_dma_buf[next_rx_buffer]),
				   CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US) != 0) {
			k_spinlock_key_t key = k_spin_lock(&state_lock);
			stats.rx_restart_errors++;
			k_spin_unlock(&state_lock, key);
		} else {
			next_rx_buffer ^= 1u;
		}
		break;
	case UART_TX_DONE:
	case UART_TX_ABORTED:
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		size_t sent = event->data.tx.len;

		if (sent > tx_inflight_len) {
			sent = tx_inflight_len;
		}
		tx_busy = false;
		if (event->type == UART_TX_ABORTED && sent < tx_inflight_len) {
			tx_dma_offset += sent;
			tx_inflight_len -= sent;
			tx_retry_active = true;
		} else {
			tx_dma_offset = 0u;
			tx_inflight_len = 0u;
			tx_retry_active = false;
		}
		start_pending_tx_locked();
		k_spin_unlock(&state_lock, key);
	}
		break;
	default:
		break;
	}
}

int uart_bridge_init(void)
{
	struct uart_config config = {
		.baudrate = CONFIG_RADIO_BRIDGE_UART_BAUDRATE,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int err;

	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	rb_byte_ring_init(&rx_ring, rx_storage, sizeof(rx_storage));
	rb_byte_ring_init(&tx_ring, tx_storage, sizeof(tx_storage));
	memset(&stats, 0, sizeof(stats));
	memset(&stats_snapshot, 0, sizeof(stats_snapshot));
	tx_busy = false;
	tx_inflight_len = 0u;
	tx_dma_offset = 0u;
	tx_retry_active = false;
	/* RX buffer zero is passed to uart_rx_enable below; buffer one is next. */
	next_rx_buffer = 1u;
	err = uart_configure(uart_dev, &config);
	if (err != 0) {
		return err;
	}
	err = uart_callback_set(uart_dev, uart_callback, NULL);
	if (err != 0) {
		return err;
	}
	return uart_rx_enable(uart_dev, rx_dma_buf[0], sizeof(rx_dma_buf[0]),
			      CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US);
}

size_t uart_bridge_read(uint8_t *data, size_t max_len)
{
	size_t read;
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	read = rb_byte_ring_read(&rx_ring, data, max_len);
	k_spin_unlock(&state_lock, key);
	return read;
}

size_t uart_bridge_write(const uint8_t *data, size_t len)
{
	size_t written;

	if (data == NULL || len == 0u) {
		return 0u;
	}
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		written = rb_byte_ring_write(&tx_ring, data, len);
		stats.tx_bytes += written;
		stats.tx_drop_bytes = tx_ring.dropped_bytes;
		start_pending_tx_locked();
		k_spin_unlock(&state_lock, key);
	}
	return written;
}

void uart_bridge_set_wake_callback(rb_uart_bridge_wake_fn cb)
{
	wake_callback = cb;
}

const struct rb_uart_stats *uart_bridge_stats_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	stats.rx_drop_bytes = rx_ring.dropped_bytes;
	stats.tx_drop_bytes = tx_ring.dropped_bytes;
	stats_snapshot = stats;
	k_spin_unlock(&state_lock, key);
	return &stats_snapshot;
}
