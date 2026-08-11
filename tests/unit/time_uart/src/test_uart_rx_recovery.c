#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "uart_rx_recovery.h"

ZTEST(uart_rx_recovery, test_backoff_saturates_at_100_ms)
{
	static const uint32_t expected[] = {
		1u, 2u, 4u, 8u, 16u, 32u, 64u, 100u, 100u,
	};
	struct rb_uart_rx_recovery recovery;

	rb_uart_rx_recovery_init(&recovery);
	for (size_t index = 0u; index < ARRAY_SIZE(expected); index++) {
		zassert_equal(rb_uart_rx_recovery_next_delay_ms(&recovery),
			      expected[index]);
	}
}

ZTEST(uart_rx_recovery, test_valid_data_resets_backoff)
{
	struct rb_uart_rx_recovery recovery;

	rb_uart_rx_recovery_init(&recovery);
	(void)rb_uart_rx_recovery_next_delay_ms(&recovery);
	(void)rb_uart_rx_recovery_next_delay_ms(&recovery);
	rb_uart_rx_recovery_reset(&recovery);
	zassert_equal(rb_uart_rx_recovery_next_delay_ms(&recovery), 1u);
}

ZTEST(uart_rx_recovery, test_null_state_is_safe)
{
	rb_uart_rx_recovery_init(NULL);
	rb_uart_rx_recovery_reset(NULL);
	zassert_equal(rb_uart_rx_recovery_next_delay_ms(NULL), 0u);
}

ZTEST_SUITE(uart_rx_recovery, NULL, NULL, NULL, NULL, NULL);
