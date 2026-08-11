#ifndef TIME_UART_H_
#define TIME_UART_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_ZTEST)
#include <zephyr/drivers/uart.h>

struct rb_time_uart_test_driver_ops {
	int (*configure)(const struct device *dev, const struct uart_config *cfg);
	int (*callback_set)(const struct device *dev, uart_callback_t callback,
			    void *user_data);
	int (*rx_enable)(const struct device *dev, uint8_t *buf, size_t len,
			 int32_t timeout);
};
#endif

struct rb_time_uart_stats {
	uint64_t rx_bytes;
	uint64_t rx_drop_bytes;
	uint32_t lines_received;
	uint32_t overlong_line_drops;
	uint32_t output_line_drops;
	uint32_t rx_restart_errors;
	uint32_t rx_buffer_errors;
	uint32_t rx_stopped_events;
	uint32_t rx_stop_reason_mask;
};

typedef void (*rb_time_uart_wake_fn)(void);

/* The UART callback owns DMA-to-ring copying. One thread at a time may call
 * time_uart_read_line(); concurrent callers receive zero bytes. */
int time_uart_init(uint32_t baudrate);
void time_uart_set_wake_callback(rb_time_uart_wake_fn callback);
size_t time_uart_read_line(char *line, size_t max_len);
const struct rb_time_uart_stats *time_uart_stats_get(void);

#if defined(CONFIG_ZTEST)
bool time_uart_test_callback_registered(void);
void time_uart_test_inject_event(struct uart_event *event);
void time_uart_test_set_driver_ops(const struct rb_time_uart_test_driver_ops *ops);
#endif

#endif /* TIME_UART_H_ */
