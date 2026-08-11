#ifndef UART_RX_RECOVERY_H_
#define UART_RX_RECOVERY_H_

#include <stdint.h>

struct rb_uart_rx_recovery {
	uint32_t next_delay_ms;
};

void rb_uart_rx_recovery_init(struct rb_uart_rx_recovery *recovery);
void rb_uart_rx_recovery_reset(struct rb_uart_rx_recovery *recovery);
uint32_t rb_uart_rx_recovery_next_delay_ms(
	struct rb_uart_rx_recovery *recovery);

#endif /* UART_RX_RECOVERY_H_ */
