#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

#define TIMEBASE_TICKS_PER_SEC 1000000u
#define TIMEBASE_RADIO_CAPTURE_CHANNEL 3u

int timebase_init(void);
uint64_t timebase_now_us(void);
uint32_t timebase_now32(void);
uint32_t timebase_ticks_until32(uint32_t target_tick);
int timebase_schedule_compare(uint8_t channel, uint32_t target_tick);
int timebase_set_compare_no_irq(uint8_t channel, uint32_t target_tick);
void timebase_cancel_compare(uint8_t channel);
uint32_t timebase_capture(uint8_t channel);
uint64_t timebase_capture64(uint8_t channel);
uint32_t timebase_timer_event_address(uint8_t channel);
uint32_t timebase_timer_capture_task_address(uint8_t channel);

typedef void (*timebase_compare_handler_t)(uint8_t channel, uint64_t now_us,
					   void *user_data);

int timebase_set_compare_handler(timebase_compare_handler_t handler,
				 void *user_data);

#endif
