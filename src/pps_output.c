#include "pps_output.h"

#include "time_sync_math.h"
#include "timebase.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hal/nrf_gpiote.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>

#define PPS_NODE DT_ALIAS(pps_out)

#if !DT_NODE_HAS_STATUS_OKAY(PPS_NODE)
#error "pps-out devicetree alias is not defined"
#endif

#define PPS_RISE_CHANNEL 1u
#define PPS_MIN_ARM_AHEAD_US 100u
#define PPS_GPIOTE_CHANNEL 0u
#define PPS_TIMER NRF_TIMER2
#define PPS_TIMER_IRQ TIMER2_IRQn
#define PPS_TIMER_IRQ_PRIORITY 2
#define PPS_TIMER_FREQ_HZ TIMEBASE_TICKS_PER_SEC
#define PPS_TIMER_PERIOD_CHANNEL 0u
#define PPS_TIMER_FALL_CHANNEL 1u

static const struct gpio_dt_spec pps_gpio = GPIO_DT_SPEC_GET(PPS_NODE, gpios);
static uint32_t pulse_width;
static uint32_t pps_period;
static struct pps_output_stats stats;
static nrfx_gppi_handle_t pps_period_set_handle;
static nrfx_gppi_handle_t pps_period_clear_handle;
static nrfx_gppi_handle_t pps_fall_handle;
static nrfx_gppi_handle_t pps_phase_reset_handle;
static nrfx_gppi_handle_t pps_phase_set_handle;
static nrfx_gppi_handle_t pps_phase_start_handle;
static bool initialized;
static bool periodic_initialized;
static bool phase_reset_initialized;
static bool periodic_started;
static bool phase_reset_pending;
static uint64_t pending_phase_tick;

static uint32_t pps_abs_pin(void)
{
	return NRF_GPIO_PIN_MAP(0, pps_gpio.pin);
}

static nrf_timer_event_t pps_timer_event(uint8_t channel)
{
	return nrf_timer_compare_event_get(channel);
}

static uint32_t pps_timer_event_address(uint8_t channel)
{
	return nrf_timer_event_address_get(PPS_TIMER, pps_timer_event(channel));
}

static uint32_t pps_timer_task_address(nrf_timer_task_t task)
{
	return nrf_timer_task_address_get(PPS_TIMER, task);
}

static void pps_timer_init(void)
{
	nrf_timer_task_trigger(PPS_TIMER, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(PPS_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(PPS_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(PPS_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(
		PPS_TIMER,
		NRF_TIMER_PRESCALER_CALCULATE(
			NRF_TIMER_BASE_FREQUENCY_GET(PPS_TIMER),
			PPS_TIMER_FREQ_HZ));
	nrf_timer_shorts_set(PPS_TIMER,
			     nrf_timer_short_compare_clear_get(
				     PPS_TIMER_PERIOD_CHANNEL));
	nrf_timer_int_enable(PPS_TIMER,
			     nrf_timer_compare_int_get(
				     PPS_TIMER_PERIOD_CHANNEL) |
				     nrf_timer_compare_int_get(
					     PPS_TIMER_FALL_CHANNEL));

	for (uint8_t channel = 0; channel < 4u; channel++) {
		nrf_timer_event_clear(PPS_TIMER, pps_timer_event(channel));
	}
}

static int pps_hardware_init(void)
{
	uint32_t rise_event = pps_timer_event_address(PPS_TIMER_PERIOD_CHANNEL);
	uint32_t fall_event = pps_timer_event_address(PPS_TIMER_FALL_CHANNEL);
	uint32_t clear_task = pps_timer_task_address(NRF_TIMER_TASK_CLEAR);
	uint32_t set_task = nrf_gpiote_task_address_get(
		NRF_GPIOTE0, nrf_gpiote_set_task_get(PPS_GPIOTE_CHANNEL));
	uint32_t clr_task = nrf_gpiote_task_address_get(
		NRF_GPIOTE0, nrf_gpiote_clr_task_get(PPS_GPIOTE_CHANNEL));
	int ret;

	nrf_gpiote_task_disable(NRF_GPIOTE0, PPS_GPIOTE_CHANNEL);
	nrf_gpiote_task_configure(NRF_GPIOTE0, PPS_GPIOTE_CHANNEL,
				  pps_abs_pin(), NRF_GPIOTE_POLARITY_NONE,
				  NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_force(NRF_GPIOTE0, PPS_GPIOTE_CHANNEL,
			      NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_enable(NRF_GPIOTE0, PPS_GPIOTE_CHANNEL);

	ret = nrfx_gppi_conn_alloc(rise_event, set_task,
				   &pps_period_set_handle);
	if (ret != 0) {
		return ret;
	}

	ret = nrfx_gppi_conn_alloc(rise_event, clear_task,
				   &pps_period_clear_handle);
	if (ret != 0) {
		nrfx_gppi_conn_free(rise_event, set_task,
				    pps_period_set_handle);
		return ret;
	}

	ret = nrfx_gppi_conn_alloc(fall_event, clr_task, &pps_fall_handle);
	if (ret != 0) {
		nrfx_gppi_conn_free(rise_event, clear_task,
				    pps_period_clear_handle);
		nrfx_gppi_conn_free(rise_event, set_task,
				    pps_period_set_handle);
		return ret;
	}

	nrfx_gppi_conn_enable(pps_period_set_handle);
	nrfx_gppi_conn_enable(pps_period_clear_handle);
	nrfx_gppi_conn_enable(pps_fall_handle);
	periodic_initialized = true;

	return 0;
}

static void pps_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (nrf_timer_event_check(PPS_TIMER,
				  pps_timer_event(PPS_TIMER_PERIOD_CHANNEL))) {
		nrf_timer_event_clear(PPS_TIMER,
				      pps_timer_event(PPS_TIMER_PERIOD_CHANNEL));
		stats.last_rise_tick = stats.scheduled_rise_tick;
		stats.pulses++;
		if (!phase_reset_pending) {
			stats.scheduled_rise_tick = time_sync_next_epoch_tick_after(
				stats.epoch_tick, pps_period,
				stats.last_rise_tick);
		}
	}

	if (nrf_timer_event_check(PPS_TIMER,
				  pps_timer_event(PPS_TIMER_FALL_CHANNEL))) {
		nrf_timer_event_clear(PPS_TIMER,
				      pps_timer_event(PPS_TIMER_FALL_CHANNEL));
	}
}

static void pps_compare_handler(uint8_t channel, uint64_t now_us,
				void *user_data)
{
	ARG_UNUSED(user_data);

	if (channel == PPS_RISE_CHANNEL) {
		ARG_UNUSED(now_us);
		stats.epoch_tick = pending_phase_tick;
		stats.last_rise_tick = pending_phase_tick;
		stats.pulses++;
		stats.scheduled_rise_tick = time_sync_next_epoch_tick_after(
			stats.epoch_tick, pps_period, stats.last_rise_tick);
		phase_reset_pending = false;
		nrfx_gppi_conn_disable(pps_phase_reset_handle);
		nrfx_gppi_conn_disable(pps_phase_set_handle);
		nrfx_gppi_conn_disable(pps_phase_start_handle);
	}
}

static int pps_phase_reset_init(void)
{
	uint32_t phase_event = timebase_timer_event_address(PPS_RISE_CHANNEL);
	uint32_t clear_task = pps_timer_task_address(NRF_TIMER_TASK_CLEAR);
	uint32_t start_task = pps_timer_task_address(NRF_TIMER_TASK_START);
	uint32_t set_task = nrf_gpiote_task_address_get(
		NRF_GPIOTE0, nrf_gpiote_set_task_get(PPS_GPIOTE_CHANNEL));
	int ret;

	ret = nrfx_gppi_conn_alloc(phase_event, clear_task,
				   &pps_phase_reset_handle);
	if (ret != 0) {
		return ret;
	}

	ret = nrfx_gppi_conn_alloc(phase_event, set_task, &pps_phase_set_handle);
	if (ret != 0) {
		nrfx_gppi_conn_free(phase_event, clear_task,
				    pps_phase_reset_handle);
		return ret;
	}

	ret = nrfx_gppi_conn_alloc(phase_event, start_task,
				   &pps_phase_start_handle);
	if (ret != 0) {
		nrfx_gppi_conn_free(phase_event, set_task,
				    pps_phase_set_handle);
		nrfx_gppi_conn_free(phase_event, clear_task,
				    pps_phase_reset_handle);
		return ret;
	}

	nrfx_gppi_conn_disable(pps_phase_reset_handle);
	nrfx_gppi_conn_disable(pps_phase_set_handle);
	nrfx_gppi_conn_disable(pps_phase_start_handle);
	phase_reset_initialized = true;
	return 0;
}

int pps_output_init(uint32_t pulse_width_us)
{
	int ret;

	if (!gpio_is_ready_dt(&pps_gpio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&pps_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	pps_timer_init();

	ret = pps_hardware_init();
	if (ret < 0) {
		return ret;
	}

	ret = pps_phase_reset_init();
	if (ret < 0) {
		return ret;
	}

	pulse_width = pulse_width_us;
	timebase_set_compare_handler(pps_compare_handler, NULL);
	IRQ_CONNECT(PPS_TIMER_IRQ, PPS_TIMER_IRQ_PRIORITY, pps_timer_isr, NULL,
		    0);
	irq_enable(PPS_TIMER_IRQ);
	initialized = true;

	return 0;
}

int pps_output_schedule(uint64_t rise_tick)
{
	uint64_t now;

	if (!initialized) {
		return -EACCES;
	}

	if (!periodic_started || pps_period == 0u) {
		return -EACCES;
	}

	now = timebase_now_us();
	if (rise_tick <= now + PPS_MIN_ARM_AHEAD_US) {
		stats.late_schedules++;
		return -ETIME;
	}

	if (phase_reset_pending && pending_phase_tick == rise_tick) {
		return 0;
	}

	pending_phase_tick = rise_tick;
	phase_reset_pending = true;
	stats.scheduled_rise_tick = rise_tick;
	if (!phase_reset_initialized) {
		phase_reset_pending = false;
		return -EACCES;
	}

	int ret = timebase_schedule_compare(PPS_RISE_CHANNEL,
					    (uint32_t)rise_tick);

	if (ret != 0) {
		phase_reset_pending = false;
		return ret;
	}

	nrfx_gppi_conn_enable(pps_phase_reset_handle);
	nrfx_gppi_conn_enable(pps_phase_set_handle);
	nrfx_gppi_conn_enable(pps_phase_start_handle);
	stats.phase_resets++;

	return 0;
}

int pps_output_start_periodic(uint64_t first_rise_tick, uint32_t period_us)
{
	int ret;

	if (!initialized) {
		return -EACCES;
	}

	if (!periodic_initialized) {
		return -EACCES;
	}

	if (period_us == 0u || pulse_width == 0u || pulse_width >= period_us) {
		return -EINVAL;
	}

	pps_period = period_us;
	nrf_timer_cc_set(PPS_TIMER, PPS_TIMER_PERIOD_CHANNEL, pps_period);
	nrf_timer_cc_set(PPS_TIMER, PPS_TIMER_FALL_CHANNEL, pulse_width);
	nrf_timer_event_clear(PPS_TIMER,
			      pps_timer_event(PPS_TIMER_PERIOD_CHANNEL));
	nrf_timer_event_clear(PPS_TIMER, pps_timer_event(PPS_TIMER_FALL_CHANNEL));
	nrf_timer_task_trigger(PPS_TIMER, NRF_TIMER_TASK_CLEAR);
	periodic_started = true;

	ret = pps_output_reset_epoch(first_rise_tick);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

int pps_output_reset_epoch(uint64_t first_rise_tick)
{
	if (!periodic_started) {
		return -EACCES;
	}

	return pps_output_schedule(first_rise_tick);
}

uint64_t pps_output_scheduled_tick(void)
{
	return stats.scheduled_rise_tick;
}

const struct pps_output_stats *pps_output_stats_get(void)
{
	return &stats;
}
