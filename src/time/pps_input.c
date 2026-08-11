#include "pps_input.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_gpiote.h>
#include <helpers/nrfx_gppi.h>
#include <gpiote_nrfx.h>

#include "timebase.h"

#define PPS_INPUT_NODE DT_ALIAS(pps_in)
#define PPS_INPUT_QUEUE_DEPTH 8u
#define PPS_INPUT_GPIOTE GPIOTE_NRFX_INST_BY_NODE(DT_NODELABEL(gpiote))

#if !DT_NODE_HAS_STATUS_OKAY(PPS_INPUT_NODE)
#error "pps-in devicetree alias is required"
#endif

static const struct gpio_dt_spec pps_input_gpio =
	GPIO_DT_SPEC_GET(PPS_INPUT_NODE, gpios);
static uint8_t pps_input_gpiote_channel;
static nrfx_gppi_handle_t pps_input_gppi_handle;
static bool initialized;
static atomic_t captured_count;
static atomic_t dropped_count;
K_MSGQ_DEFINE(pps_input_queue, sizeof(uint64_t), PPS_INPUT_QUEUE_DEPTH, 8);

static uint32_t pps_input_abs_pin(void)
{
	return NRF_GPIO_PIN_MAP(0, pps_input_gpio.pin);
}

/* Called by gpio_nrfx's shared GPIOTE ISR after GPPI has captured CC4. */
static void pps_input_isr(nrfx_gpiote_pin_t pin,
			  nrfx_gpiote_trigger_t trigger, void *context)
{
	uint64_t capture_tick;

	ARG_UNUSED(context);
	if (pin != pps_input_abs_pin() || trigger != NRFX_GPIOTE_TRIGGER_LOTOHI) {
		return;
	}
	capture_tick = timebase_capture64(TIMEBASE_PPS_INPUT_CAPTURE_CHANNEL);
	if (k_msgq_put(&pps_input_queue, &capture_tick, K_NO_WAIT) == 0) {
		atomic_inc(&captured_count);
	} else {
		atomic_inc(&dropped_count);
	}
}

int pps_input_init(void)
{
	nrfx_gpiote_handler_config_t handler = {
		.handler = pps_input_isr,
		.p_context = NULL,
	};
	nrfx_gpiote_trigger_config_t trigger = {
		.trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
		.p_in_channel = &pps_input_gpiote_channel,
	};
	nrfx_gpiote_input_pin_config_t config = {
		.p_pull_config = NULL,
		.p_trigger_config = &trigger,
		.p_handler_config = &handler,
	};
	nrf_gpiote_event_t event;
	uint32_t event_address;
	int err;

	if (initialized) {
		return 0;
	}
	if (!device_is_ready(pps_input_gpio.port)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&pps_input_gpio, GPIO_INPUT);
	if (err != 0) {
		return err;
	}
	err = nrfx_gpiote_channel_alloc(&PPS_INPUT_GPIOTE,
				       &pps_input_gpiote_channel);
	if (err != 0) {
		return err;
	}
	err = nrfx_gpiote_input_configure(&PPS_INPUT_GPIOTE,
					 pps_input_abs_pin(), &config);
	if (err != 0) {
		(void)nrfx_gpiote_channel_free(&PPS_INPUT_GPIOTE,
					       pps_input_gpiote_channel);
		return err;
	}
	event = nrf_gpiote_in_event_get(pps_input_gpiote_channel);
	event_address = nrf_gpiote_event_address_get(NRF_GPIOTE0, event);
	err = nrfx_gppi_conn_alloc(event_address,
		timebase_timer_capture_task_address(
			TIMEBASE_PPS_INPUT_CAPTURE_CHANNEL), &pps_input_gppi_handle);
	if (err != 0) {
		nrfx_gpiote_trigger_disable(&PPS_INPUT_GPIOTE, pps_input_abs_pin());
		(void)nrfx_gpiote_pin_uninit(&PPS_INPUT_GPIOTE, pps_input_abs_pin());
		(void)nrfx_gpiote_channel_free(&PPS_INPUT_GPIOTE,
					       pps_input_gpiote_channel);
		return err;
	}
	k_msgq_purge(&pps_input_queue);
	atomic_set(&captured_count, 0);
	atomic_set(&dropped_count, 0);
	nrfx_gppi_conn_enable(pps_input_gppi_handle);
	nrfx_gpiote_trigger_enable(&PPS_INPUT_GPIOTE, pps_input_abs_pin(), true);
	initialized = true;
	return 0;
}

int pps_input_poll(uint64_t *capture_tick, k_timeout_t timeout)
{
	if (capture_tick == NULL) {
		return -EINVAL;
	}
	return k_msgq_get(&pps_input_queue, capture_tick, timeout);
}

uint64_t pps_input_count(void)
{
	return (uint64_t)atomic_get(&captured_count);
}

uint64_t pps_input_drop_count(void)
{
	return (uint64_t)atomic_get(&dropped_count);
}
