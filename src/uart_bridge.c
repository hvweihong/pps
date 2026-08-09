#include "uart_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "byte_ring.h"
#include "record_queue.h"

#if !DT_NODE_HAS_STATUS(DT_ALIAS(bridge_uart), okay)
#error "bridge-uart alias is required"
#endif

#define RB_UART_DEVICE DT_ALIAS(bridge_uart)
#define RB_UART_RX_BUFFER_SIZE 512u
#define RB_UART_TX_BUFFER_SIZE 512u
#define RB_UART_TX_RECORD_CAPACITY 64u

static const struct device *const uart_dev = DEVICE_DT_GET(RB_UART_DEVICE);
static uint8_t rx_dma_buf[2][RB_UART_RX_BUFFER_SIZE];
static uint8_t tx_dma_buf[RB_UART_TX_BUFFER_SIZE];
static uint8_t rx_storage[CONFIG_RADIO_BRIDGE_UART_RING_SIZE];
static uint8_t tx_storage[CONFIG_RADIO_BRIDGE_UART_RING_SIZE];
static uint16_t tx_record_lengths[RB_UART_TX_RECORD_CAPACITY];
static struct rb_byte_ring rx_ring;
static struct rb_record_queue tx_record_queue;
static struct rb_uart_stats stats;
static struct rb_uart_stats stats_snapshot;
static struct k_spinlock state_lock;
static bool tx_busy;
static bool tx_record_loaded;
static size_t tx_record_len;
static size_t tx_dma_offset;
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
	size_t record_len;
	size_t remaining;
	int err;

	if (tx_busy) {
		return;
	}
	if (!tx_record_loaded) {
		err = rb_record_queue_peek(&tx_record_queue, tx_dma_buf,
					   sizeof(tx_dma_buf), &record_len);
		if (err == -EAGAIN) {
			return;
		}
		if (err != 0) {
			stats.tx_start_errors++;
			return;
		}
		tx_record_loaded = true;
		tx_record_len = record_len;
		tx_dma_offset = 0u;
	}
	remaining = tx_record_len - tx_dma_offset;
	err = uart_tx(uart_dev, tx_dma_buf + tx_dma_offset, remaining, 0);
	if (err == 0) {
		tx_busy = true;
	} else {
		stats.tx_start_errors++;
	}
}

static void complete_tx_record_locked(void)
{
	if (rb_record_queue_pop(&tx_record_queue) == 0) {
		stats.tx_record_completed++;
	}
	tx_record_loaded = false;
	tx_record_len = 0u;
	tx_dma_offset = 0u;
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
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);

		tx_busy = false;
		complete_tx_record_locked();
		start_pending_tx_locked();
		k_spin_unlock(&state_lock, key);
		if (wake_callback != NULL) {
			wake_callback();
		}
	}
		break;
	case UART_TX_ABORTED:
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		size_t remaining = tx_record_len - tx_dma_offset;
		size_t sent = event->data.tx.len;

		if (sent > remaining) {
			sent = remaining;
		}
		tx_busy = false;
		tx_dma_offset += sent;
		stats.tx_record_aborted++;
		if (tx_dma_offset == tx_record_len) {
			complete_tx_record_locked();
		}
		start_pending_tx_locked();
		k_spin_unlock(&state_lock, key);
		if (wake_callback != NULL) {
			wake_callback();
		}
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
	rb_record_queue_init(&tx_record_queue, tx_storage, sizeof(tx_storage),
			     tx_record_lengths, RB_UART_TX_RECORD_CAPACITY);
	memset(&stats, 0, sizeof(stats));
	memset(&stats_snapshot, 0, sizeof(stats_snapshot));
	tx_busy = false;
	tx_record_loaded = false;
	tx_record_len = 0u;
	tx_dma_offset = 0u;
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
	size_t written = 0u;

	if (data == NULL || len == 0u) {
		return 0u;
	}
	while (written < len) {
		size_t available = uart_bridge_record_available();
		size_t chunk_len = len - written;

		if (available == 0u) {
			break;
		}
		if (chunk_len > available) {
			chunk_len = available;
		}
		if (uart_bridge_write_record(data + written, chunk_len) != 0) {
			break;
		}
		written += chunk_len;
	}
	return written;
}

int uart_bridge_write_record(const uint8_t *data, size_t len)
{
	int ret;

	if (data == NULL || len == 0u) {
		return -EINVAL;
	}
	if (len > sizeof(tx_dma_buf)) {
		return -EMSGSIZE;
	}
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);

		ret = rb_record_queue_push(&tx_record_queue, data, len);
		if (ret == 0) {
			stats.tx_bytes += len;
			stats.tx_record_queued++;
			start_pending_tx_locked();
		} else if (ret == -ENOSPC) {
			stats.tx_record_rejected++;
		}
		k_spin_unlock(&state_lock, key);
	}
	return ret;
}

size_t uart_bridge_record_available(void)
{
	size_t available;
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	if (tx_record_queue.length_count == tx_record_queue.length_capacity) {
		available = 0u;
	} else {
		available = tx_record_queue.storage_size - tx_record_queue.data_count;
		if (available > sizeof(tx_dma_buf)) {
			available = sizeof(tx_dma_buf);
		}
	}
	k_spin_unlock(&state_lock, key);
	return available;
}

void uart_bridge_set_wake_callback(rb_uart_bridge_wake_fn cb)
{
	wake_callback = cb;
}

const struct rb_uart_stats *uart_bridge_stats_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	stats.rx_drop_bytes = rx_ring.dropped_bytes;
	stats_snapshot = stats;
	stats_snapshot.tx_record_pending = tx_record_queue.length_count;
	stats_snapshot.tx_record_busy = tx_busy ? 1u : 0u;
	k_spin_unlock(&state_lock, key);
	return &stats_snapshot;
}
