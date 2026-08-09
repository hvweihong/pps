#ifndef PPS_INPUT_H_
#define PPS_INPUT_H_

#include <stdint.h>

#include <zephyr/kernel.h>

/* Thread-facing PPS capture handoff. The ISR producer is bounded and never
 * performs clock discipline, NMEA parsing, or settings access. */
int pps_input_init(void);
int pps_input_poll(uint64_t *capture_tick, k_timeout_t timeout);
uint64_t pps_input_count(void);
uint64_t pps_input_drop_count(void);

#endif /* PPS_INPUT_H_ */
