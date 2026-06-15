#include "timebase.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hal/nrf_clock.h>
#include <hal/nrf_timer.h>
#include <nrfx.h>

#define TIMEBASE_TIMER NRF_TIMER3
#define TIMEBASE_TIMER_IRQ TIMER3_IRQn
#define TIMEBASE_TIMER_IRQ_PRIORITY 1
#define TIMEBASE_TIMER_FREQ_HZ TIMEBASE_TICKS_PER_SEC
#define TIMEBASE_CAPTURE_CHANNEL 0u
#define TIMEBASE_COMPARE_FIRST_CHANNEL 1u
#define TIMEBASE_COMPARE_LAST_CHANNEL 2u
#define TIMEBASE_PPI_CHANNEL 3u
#define TIMEBASE_CHANNEL_COUNT 4u
#define TIMEBASE_COMPARE_MASK(ch) nrf_timer_compare_int_get(ch)

static volatile uint32_t high_word;
static uint32_t last_low_word;
static timebase_compare_handler_t compare_handler;
static void *compare_handler_data;
static bool initialized;

static void hfclk_start(void)
{
	nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
	while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED)) {
	}
}

static uint64_t compose_now(uint32_t high, uint32_t low)
{
	return ((uint64_t)high << 32) | low;
}

static uint64_t extend_low_word_locked(uint32_t low)
{
	if (low < last_low_word) {
		high_word++;
	}

	last_low_word = low;
	return compose_now(high_word, low);
}

static void timebase_isr(const void *arg)
{
	ARG_UNUSED(arg);

	for (uint8_t channel = TIMEBASE_COMPARE_FIRST_CHANNEL;
	     channel <= TIMEBASE_COMPARE_LAST_CHANNEL; channel++) {
		nrf_timer_event_t event =
			nrf_timer_compare_event_get(channel);

		if (!nrf_timer_event_check(TIMEBASE_TIMER, event)) {
			continue;
		}

		nrf_timer_event_clear(TIMEBASE_TIMER, event);
		nrf_timer_int_disable(TIMEBASE_TIMER, TIMEBASE_COMPARE_MASK(channel));

		if (compare_handler != NULL) {
			compare_handler(channel, timebase_now_us(),
					compare_handler_data);
		}
	}
}

int timebase_init(void)
{
	if (initialized) {
		return 0;
	}

	hfclk_start();

	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(TIMEBASE_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TIMEBASE_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(
		TIMEBASE_TIMER,
		NRF_TIMER_PRESCALER_CALCULATE(
			NRF_TIMER_BASE_FREQUENCY_GET(TIMEBASE_TIMER),
			TIMEBASE_TIMER_FREQ_HZ));
	nrf_timer_shorts_set(TIMEBASE_TIMER, 0);

	for (uint8_t channel = 0; channel < TIMEBASE_CHANNEL_COUNT; channel++) {
		nrf_timer_event_t event =
			nrf_timer_compare_event_get(channel);

		nrf_timer_event_clear(TIMEBASE_TIMER, event);
	}

	IRQ_CONNECT(TIMEBASE_TIMER_IRQ, TIMEBASE_TIMER_IRQ_PRIORITY, timebase_isr,
		    NULL, 0);
	irq_enable(TIMEBASE_TIMER_IRQ);

	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_START);

	initialized = true;
	return 0;
}

uint32_t timebase_now32(void)
{
	unsigned int key = irq_lock();

	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_CAPTURE0);
	uint32_t now = nrf_timer_cc_get(TIMEBASE_TIMER,
					TIMEBASE_CAPTURE_CHANNEL);

	irq_unlock(key);
	return now;
}

uint64_t timebase_now_us(void)
{
	unsigned int key = irq_lock();

	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_CAPTURE0);
	uint32_t low = nrf_timer_cc_get(TIMEBASE_TIMER,
					TIMEBASE_CAPTURE_CHANNEL);
	uint64_t now = extend_low_word_locked(low);

	irq_unlock(key);
	return now;
}

uint32_t timebase_ticks_until32(uint32_t target_tick)
{
	return target_tick - timebase_now32();
}

int timebase_schedule_compare(uint8_t channel, uint32_t target_tick)
{
	if (channel < TIMEBASE_COMPARE_FIRST_CHANNEL ||
	    channel > TIMEBASE_COMPARE_LAST_CHANNEL) {
		return -EINVAL;
	}

	nrf_timer_event_t event =
		nrf_timer_compare_event_get(channel);

	nrf_timer_event_clear(TIMEBASE_TIMER, event);
	nrf_timer_cc_set(TIMEBASE_TIMER, channel, target_tick);
	nrf_timer_int_enable(TIMEBASE_TIMER, TIMEBASE_COMPARE_MASK(channel));

	return 0;
}

int timebase_set_compare_no_irq(uint8_t channel, uint32_t target_tick)
{
	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return -EINVAL;
	}

	nrf_timer_event_t event =
		nrf_timer_compare_event_get(channel);

	nrf_timer_int_disable(TIMEBASE_TIMER, TIMEBASE_COMPARE_MASK(channel));
	nrf_timer_event_clear(TIMEBASE_TIMER, event);
	nrf_timer_cc_set(TIMEBASE_TIMER, channel, target_tick);

	return 0;
}

void timebase_cancel_compare(uint8_t channel)
{
	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return;
	}

	nrf_timer_int_disable(TIMEBASE_TIMER, TIMEBASE_COMPARE_MASK(channel));
}

uint32_t timebase_capture(uint8_t channel)
{
	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return timebase_now32();
	}

	nrf_timer_task_t task = nrf_timer_capture_task_get(channel);

	nrf_timer_task_trigger(TIMEBASE_TIMER, task);
	return nrf_timer_cc_get(TIMEBASE_TIMER, channel);
}

uint64_t timebase_capture64(uint8_t channel)
{
	uint32_t captured;
	uint64_t now;
	uint32_t high;
	uint32_t now_low;
	unsigned int key;

	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return timebase_now_us();
	}

	key = irq_lock();
	captured = nrf_timer_cc_get(TIMEBASE_TIMER, channel);
	nrf_timer_task_trigger(TIMEBASE_TIMER, NRF_TIMER_TASK_CAPTURE0);
	now_low = nrf_timer_cc_get(TIMEBASE_TIMER, TIMEBASE_CAPTURE_CHANNEL);
	now = extend_low_word_locked(now_low);
	irq_unlock(key);

	high = (uint32_t)(now >> 32);

	if (captured > now_low && high > 0u) {
		high--;
	}

	return compose_now(high, captured);
}

uint32_t timebase_timer_event_address(uint8_t channel)
{
	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return 0;
	}

	nrf_timer_event_t event =
		nrf_timer_compare_event_get(channel);

	return nrf_timer_event_address_get(TIMEBASE_TIMER, event);
}

uint32_t timebase_timer_capture_task_address(uint8_t channel)
{
	if (channel >= TIMEBASE_CHANNEL_COUNT) {
		return 0;
	}

	return nrf_timer_task_address_get(TIMEBASE_TIMER,
					  nrf_timer_capture_task_get(channel));
}

int timebase_set_compare_handler(timebase_compare_handler_t handler,
				 void *user_data)
{
	compare_handler = handler;
	compare_handler_data = user_data;
	return 0;
}
