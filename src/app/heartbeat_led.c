#include "heartbeat_led.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(heartbeat_led, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define HEARTBEAT_STACK_SIZE 512
#define HEARTBEAT_PRIORITY 7

#if !DT_NODE_HAS_STATUS_OKAY(LED0_NODE)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static K_THREAD_STACK_DEFINE(heartbeat_stack, HEARTBEAT_STACK_SIZE);
static struct k_thread heartbeat_thread;
static atomic_t heartbeat_period_ms = ATOMIC_INIT(HEARTBEAT_LED_DEFAULT_PERIOD_MS);
static atomic_t heartbeat_enabled;
static bool started;

static void heartbeat_thread_fn(void *arg1, void *arg2, void *arg3)
{
	bool led_on = true;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		if (atomic_get(&heartbeat_enabled) != 0) {
			gpio_pin_set_dt(&led, led_on);
			led_on = !led_on;
			k_sleep(K_MSEC(atomic_get(&heartbeat_period_ms)));
		} else {
			if (led_on == false) {
				gpio_pin_set_dt(&led, false);
				led_on = true;
			}
			k_sleep(K_MSEC(100));
		}
	}
}

int heartbeat_led_start(bool enabled, uint32_t period_ms)
{
	int ret;

	if (started) {
		return 0;
	}

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}

	if (period_ms == 0u) {
		return -EINVAL;
	}
	atomic_set(&heartbeat_period_ms, period_ms);
	atomic_set(&heartbeat_enabled, enabled);

	k_thread_create(&heartbeat_thread, heartbeat_stack,
			K_THREAD_STACK_SIZEOF(heartbeat_stack),
			heartbeat_thread_fn, NULL, NULL, NULL,
			HEARTBEAT_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&heartbeat_thread, "heartbeat_led");
	started = true;

	LOG_INF("heartbeat LED enabled=%u period=%ums", enabled, period_ms);
	return 0;
}
