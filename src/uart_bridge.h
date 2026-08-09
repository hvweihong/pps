#ifndef UART_BRIDGE_H_
#define UART_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

struct rb_uart_stats {
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_drop_bytes;
	uint64_t tx_drop_bytes;
	uint64_t tx_record_queued;
	uint64_t tx_record_completed;
	uint64_t tx_record_aborted;
	uint64_t tx_record_rejected;
	uint32_t tx_record_pending;
	uint32_t tx_start_errors;
	uint8_t tx_record_busy;
	uint32_t rx_disabled_events;
	uint32_t rx_restart_errors;
};

typedef void (*rb_uart_bridge_wake_fn)(void);

int uart_bridge_init(void);
size_t uart_bridge_read(uint8_t *data, size_t max_len);
size_t uart_bridge_write(const uint8_t *data, size_t len);
int uart_bridge_write_record(const uint8_t *data, size_t len);
size_t uart_bridge_record_available(void);
const struct rb_uart_stats *uart_bridge_stats_get(void);
/* cb is invoked from UART ISR context whenever new RX bytes are captured, so
 * the bridge thread can react before its fallback poll timeout elapses.  cb
 * itself must be ISR-safe (e.g. a K_NO_WAIT k_msgq_put()); it must not log,
 * block, or allocate. */
void uart_bridge_set_wake_callback(rb_uart_bridge_wake_fn cb);

#endif /* UART_BRIDGE_H_ */
