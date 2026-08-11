#include "uart_rx_recovery.h"

#include <stddef.h>

#define RB_UART_RX_RECOVERY_INITIAL_DELAY_MS 1u
#define RB_UART_RX_RECOVERY_MAX_DELAY_MS 100u

void rb_uart_rx_recovery_init(struct rb_uart_rx_recovery *recovery)
{
	rb_uart_rx_recovery_reset(recovery);
}

void rb_uart_rx_recovery_reset(struct rb_uart_rx_recovery *recovery)
{
	if (recovery != NULL) {
		recovery->next_delay_ms = RB_UART_RX_RECOVERY_INITIAL_DELAY_MS;
	}
}

uint32_t rb_uart_rx_recovery_next_delay_ms(
	struct rb_uart_rx_recovery *recovery)
{
	uint32_t delay_ms;

	if (recovery == NULL) {
		return 0u;
	}
	delay_ms = recovery->next_delay_ms;
	if (delay_ms == 0u) {
		delay_ms = RB_UART_RX_RECOVERY_INITIAL_DELAY_MS;
	}
	if (delay_ms < RB_UART_RX_RECOVERY_MAX_DELAY_MS) {
		uint32_t next_delay_ms = delay_ms * 2u;

		recovery->next_delay_ms =
			next_delay_ms < RB_UART_RX_RECOVERY_MAX_DELAY_MS ?
			next_delay_ms : RB_UART_RX_RECOVERY_MAX_DELAY_MS;
	} else {
		recovery->next_delay_ms = RB_UART_RX_RECOVERY_MAX_DELAY_MS;
	}
	return delay_ms;
}
