#ifndef HEARTBEAT_LED_H
#define HEARTBEAT_LED_H

#include <stdbool.h>
#include <stdint.h>

#define HEARTBEAT_LED_DEFAULT_PERIOD_MS 1000u

int heartbeat_led_start(bool enabled, uint32_t period_ms);
int heartbeat_led_update(bool enabled, uint32_t period_ms);

#endif
