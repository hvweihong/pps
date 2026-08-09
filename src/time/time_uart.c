#include "time_uart.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "byte_ring.h"

#define TIME_UART_NODE DT_ALIAS(time_uart)
#define TIME_UART_RX_DMA_BUFFER_SIZE 256u
#define TIME_UART_RX_RING_SIZE 1024u
#define TIME_UART_LINE_SIZE 128u

#if !DT_NODE_HAS_STATUS_OKAY(TIME_UART_NODE)
#error "time-uart devicetree alias is required"
#endif

static const struct device *const uart_dev = DEVICE_DT_GET(TIME_UART_NODE);
static uint8_t rx_dma_buf[2][TIME_UART_RX_DMA_BUFFER_SIZE];
static uint8_t rx_storage[TIME_UART_RX_RING_SIZE];
static char line_storage[TIME_UART_LINE_SIZE];
static struct rb_byte_ring rx_ring;
static struct rb_time_uart_stats stats;
static struct rb_time_uart_stats stats_snapshot;
static struct k_spinlock state_lock;
K_MUTEX_DEFINE(line_lock);
static rb_time_uart_wake_fn wake_callback;
static uint8_t next_rx_buffer;
static size_t line_len;
static bool line_overflow;
static bool initialized;
static bool callback_registered;

#if defined(CONFIG_ZTEST)
static const struct rb_time_uart_test_driver_ops *test_driver_ops;
#endif

static int driver_configure(const struct uart_config *config)
{
#if defined(CONFIG_ZTEST)
	if (test_driver_ops != NULL) {
		return test_driver_ops->configure(uart_dev, config);
	}
#endif
	return uart_configure(uart_dev, config);
}

static int driver_callback_set(uart_callback_t callback)
{
#if defined(CONFIG_ZTEST)
	if (test_driver_ops != NULL) {
		return test_driver_ops->callback_set(uart_dev, callback, NULL);
	}
#endif
	return uart_callback_set(uart_dev, callback, NULL);
}

static int driver_rx_enable(uint8_t *buffer, size_t length)
{
#if defined(CONFIG_ZTEST)
	if (test_driver_ops != NULL) {
		return test_driver_ops->rx_enable(uart_dev, buffer, length, 1000u);
	}
#endif
	return uart_rx_enable(uart_dev, buffer, length, 1000u);
}

static void stats_increment(uint32_t *counter)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	(*counter)++;
	k_spin_unlock(&state_lock, key);
}

static void copy_rx_locked(const uint8_t *data, size_t length)
{
	size_t accepted;

	if (data == NULL || length == 0u) {
		return;
	}
	accepted = rb_byte_ring_write(&rx_ring, data, length);
	stats.rx_bytes += accepted;
	stats.rx_drop_bytes = rx_ring.dropped_bytes;
}

static void copy_event_rx_locked(const struct uart_event_rx *rx)
{
	if (rx->buf == NULL || rx->len == 0u) {
		return;
	}
	copy_rx_locked(rx->buf + rx->offset, rx->len);
}

/* Async UART callback: DMA bytes only; line framing remains thread context. */
static void time_uart_callback(const struct device *dev,
			       struct uart_event *event, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (event->type) {
	case UART_RX_RDY:
	{
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		copy_event_rx_locked(&event->data.rx);
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

		stats.rx_stopped_events++;
		stats.rx_stop_reason_mask |= event->data.rx_stop.reason;
		copy_event_rx_locked(rx);
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
		} else {
			stats_increment(&stats.rx_buffer_errors);
		}
		break;
	case UART_RX_DISABLED:
		if (uart_rx_enable(uart_dev, rx_dma_buf[next_rx_buffer],
				   sizeof(rx_dma_buf[next_rx_buffer]), 1000u) == 0) {
			next_rx_buffer ^= 1u;
		} else {
			stats_increment(&stats.rx_restart_errors);
		}
		break;
	default:
		break;
	}
}

static int clear_callback(void)
{
	int err;

	if (!callback_registered) {
		return 0;
	}
	err = driver_callback_set(NULL);
	if (err == 0) {
		callback_registered = false;
	}
	return err;
}

int time_uart_init(uint32_t baudrate)
{
	struct uart_config config = {
		.baudrate = baudrate,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int err;

	if (baudrate < 1200u || baudrate > 115200u) {
		return -EINVAL;
	}
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	if (initialized) {
		return -EALREADY;
	}
	/* A prior rx_enable failure may have registered a callback. */
	err = clear_callback();
	if (err != 0) {
		return err;
	}
	rb_byte_ring_init(&rx_ring, rx_storage, sizeof(rx_storage));
	memset(&stats, 0, sizeof(stats));
	line_len = 0u;
	line_overflow = false;
	next_rx_buffer = 1u;
	err = driver_configure(&config);
	if (err != 0) {
		return err;
	}
	err = driver_callback_set(time_uart_callback);
	if (err != 0) {
		return err;
	}
	callback_registered = true;
	err = driver_rx_enable(rx_dma_buf[0], sizeof(rx_dma_buf[0]));
	if (err != 0) {
		int cleanup_err;

		stats_increment(&stats.rx_restart_errors);
		cleanup_err = clear_callback();
		if (cleanup_err != 0) {
			return cleanup_err;
		}
		return err;
	}
	initialized = true;
	return 0;
}

#if defined(CONFIG_ZTEST)
bool time_uart_test_callback_registered(void)
{
	return callback_registered;
}

void time_uart_test_inject_event(struct uart_event *event)
{
	time_uart_callback(uart_dev, event, NULL);
}

void time_uart_test_set_driver_ops(const struct rb_time_uart_test_driver_ops *ops)
{
	test_driver_ops = ops;
}
#endif

void time_uart_set_wake_callback(rb_time_uart_wake_fn callback)
{
	wake_callback = callback;
}

size_t time_uart_read_line(char *line, size_t max_len)
{
	uint8_t character;

	if (line == NULL || max_len == 0u ||
	    k_mutex_lock(&line_lock, K_NO_WAIT) != 0) {
		return 0u;
	}
	while (true) {
		k_spinlock_key_t key = k_spin_lock(&state_lock);
		bool available = rb_byte_ring_read(&rx_ring, &character, 1u) == 1u;
		k_spin_unlock(&state_lock, key);

		if (!available) {
			break;
		}
		if (character == '\r' || character == '\n') {
			if (line_overflow) {
				stats_increment(&stats.overlong_line_drops);
				line_overflow = false;
				line_len = 0u;
				continue;
			}
			if (line_len == 0u) {
				continue;
			}
			if (line_len >= max_len) {
				stats_increment(&stats.output_line_drops);
				line_len = 0u;
				continue;
			}
			memcpy(line, line_storage, line_len);
			line[line_len] = '\0';
			size_t result = line_len;
			line_len = 0u;
			stats_increment(&stats.lines_received);
			k_mutex_unlock(&line_lock);
			return result;
		}
		if (line_overflow) {
			continue;
		}
		if (line_len + 1u >= sizeof(line_storage)) {
			line_overflow = true;
			continue;
		}
		line_storage[line_len++] = (char)character;
	}
	k_mutex_unlock(&line_lock);
	return 0u;
}

const struct rb_time_uart_stats *time_uart_stats_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&state_lock);

	stats.rx_drop_bytes = rx_ring.dropped_bytes;
	stats_snapshot = stats;
	k_spin_unlock(&state_lock, key);
	return &stats_snapshot;
}
