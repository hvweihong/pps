#include <errno.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#include "time_uart.h"

static int test_rx_enable_result;
static uart_callback_t test_callback;

static int test_configure(const struct device *dev, const struct uart_config *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return 0;
}

static int test_callback_set(const struct device *dev, uart_callback_t callback,
			     void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	test_callback = callback;
	return 0;
}

static int test_rx_enable(const struct device *dev, uint8_t *buffer, size_t length,
			  int32_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(buffer);
	ARG_UNUSED(length);
	ARG_UNUSED(timeout);
	return test_rx_enable_result;
}

static const struct rb_time_uart_test_driver_ops test_driver_ops = {
	.configure = test_configure,
	.callback_set = test_callback_set,
	.rx_enable = test_rx_enable,
};

ZTEST(time_uart, test_init_failure_clears_callback_before_retry)
{
	time_uart_test_set_driver_ops(&test_driver_ops);
	test_rx_enable_result = -EBUSY;
	test_callback = NULL;
	zassert_equal(time_uart_init(9600u), -EBUSY);
	zassert_false(time_uart_test_callback_registered());
	zassert_is_null(test_callback);
	test_rx_enable_result = 0;
	zassert_equal(time_uart_init(9600u), 0);
	zassert_true(time_uart_test_callback_registered());
	zassert_not_null(test_callback);
	time_uart_test_set_driver_ops(NULL);
}

ZTEST(time_uart, test_stopped_event_with_no_rx_data_counts_reason_safely)
{
	struct uart_event event = {
		.type = UART_RX_STOPPED,
		.data.rx_stop.reason = UART_ERROR_OVERRUN,
	};
	const struct rb_time_uart_stats *stats;

	time_uart_test_inject_event(&event);
	stats = time_uart_stats_get();
	zassert_equal(stats->rx_stopped_events, 1u);
	zassert_true((stats->rx_stop_reason_mask & UART_ERROR_OVERRUN) != 0u);
	zassert_equal(stats->rx_bytes, 0u);
}

ZTEST_SUITE(time_uart, NULL, NULL, NULL, NULL, NULL);
