#ifndef HEARTBEAT_LED_H
#define HEARTBEAT_LED_H

#include <stdint.h>

#define HEARTBEAT_LED_DEFAULT_PERIOD_MS 1000u

int heartbeat_led_start(uint32_t period_ms);

#endif
