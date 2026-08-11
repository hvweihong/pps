/*
 * Parameter Definitions for Radio Bridge
 *
 * Static parameter table declarations and type definitions.
 */

#ifndef PARAM_DEFS_H_
#define PARAM_DEFS_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parameter value types
 */
enum rb_param_type {
	RB_PARAM_UINT8,
	RB_PARAM_UINT16,
	RB_PARAM_UINT32,
	RB_PARAM_BYTES,
};

/**
 * Parameter behavior flags
 */
enum rb_param_flags {
	RB_PARAM_FLAG_NONE = 0,
	RB_PARAM_FLAG_REBOOT_REQUIRED = (1 << 0),  /**< Requires reboot to take effect */
};

/**
 * Parameter descriptor - defines a single configurable parameter
 */
struct rb_param_descriptor {
	const char *name;                    /**< Short CLI name (e.g., "uart_baudrate") */
	const char *description;             /**< Human-readable description */
	enum rb_param_type type;             /**< Value type */
	uint32_t flags;                      /**< Combination of rb_param_flags */
	union {
		struct {
			uint32_t min;
			uint32_t max;
			uint32_t default_value;
		} u32;
		struct {
			uint16_t min;
			uint16_t max;
			uint16_t default_value;
		} u16;
		struct {
			uint8_t min;
			uint8_t max;
			uint8_t default_value;
		} u8;
		struct {
			size_t length;
			const uint8_t *default_value;
		} bytes;
	} config;
	uint16_t nvs_id;                     /**< Unique NVS identifier (base 0x1000) */
};

/**
 * Parameter identifiers (enum for type safety)
 */
enum rb_param_id {
	RB_PARAM_ROLE_ID = 0,
	RB_PARAM_GROUP_ID,
	RB_PARAM_GROUP_KEY,
	RB_PARAM_UART_BAUDRATE,
	RB_PARAM_TIME_SOURCE_MODE,
	RB_PARAM_TIME_UART_BAUDRATE,
	RB_PARAM_PPS_INPUT_DELAY_US,
	RB_PARAM_RADIO_DELAY_US,
	RB_PARAM_COUNT  /**< Total parameter count */
};

/**
 * Global parameter table (defined in param_config.c)
 */
extern const struct rb_param_descriptor rb_param_table[RB_PARAM_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* PARAM_DEFS_H_ */
