/*
 * Parameter Configuration API for Radio Bridge
 *
 * Runtime parameter management with NVS persistence.
 */

#ifndef PARAM_CONFIG_H_
#define PARAM_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>
#include "param_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize parameter configuration subsystem.
 *
 * Loads all persisted parameters from NVS. Falls back to Kconfig
 * defaults for parameters not found in NVS.
 *
 * @return 0 on success, negative errno on failure.
 */
int rb_param_config_init(void);

/**
 * @brief Get parameter value as uint32_t.
 *
 * Works for all numeric types (uint8/uint16/uint32). Bool returns 0/1.
 * Priority: NVS persisted > runtime override > Kconfig default.
 *
 * @param id Parameter identifier
 * @param value Output pointer for parameter value
 * @return 0 on success, -EINVAL if id is invalid or value is NULL
 */
int rb_param_get_uint32(enum rb_param_id id, uint32_t *value);

/**
 * @brief Get parameter value as bool.
 *
 * Only works for RB_PARAM_BOOL type parameters.
 *
 * @param id Parameter identifier
 * @param value Output pointer for parameter value
 * @return 0 on success, -EINVAL if id is invalid or not boolean type
 */
int rb_param_get_bool(enum rb_param_id id, bool *value);

/**
 * @brief Set and persist parameter value.
 *
 * Validates range and type, then persists to NVS. Updates runtime
 * value immediately if RB_PARAM_FLAG_RUNTIME_UPDATE is set.
 *
 * @param id Parameter identifier
 * @param value New parameter value
 * @return 0 on success, -EINVAL if out of range, -EIO on NVS error
 */
int rb_param_set_uint32(enum rb_param_id id, uint32_t value);

/**
 * @brief Set and persist boolean parameter.
 *
 * Only works for RB_PARAM_BOOL type parameters.
 *
 * @param id Parameter identifier
 * @param value New parameter value
 * @return 0 on success, -EINVAL if not boolean type
 */
int rb_param_set_bool(enum rb_param_id id, bool value);

/**
 * @brief Get parameter value as byte array.
 *
 * Works for RB_PARAM_BYTES type parameters (e.g. group_key).
 *
 * @param id Parameter identifier
 * @param buffer Output buffer for parameter bytes
 * @param length Input: buffer size, Output: actual bytes copied
 * @return 0 on success, -EINVAL if id is invalid or not bytes type, -ENOSPC if buffer too small
 */
int rb_param_get_bytes(enum rb_param_id id, uint8_t *buffer, size_t *length);

/**
 * @brief Set and persist byte array parameter.
 *
 * Only works for RB_PARAM_BYTES type parameters.
 *
 * @param id Parameter identifier
 * @param buffer Input buffer containing parameter bytes
 * @param length Number of bytes to write
 * @return 0 on success, -EINVAL if not bytes type or length mismatch
 */
int rb_param_set_bytes(enum rb_param_id id, const uint8_t *buffer, size_t length);

/**
 * @brief Clear persisted parameter, restore Kconfig default.
 *
 * Removes parameter from NVS. Next read returns Kconfig default.
 *
 * @param id Parameter identifier
 * @return 0 on success, negative errno on failure
 */
int rb_param_clear(enum rb_param_id id);

/**
 * @brief Clear all persisted parameters (factory reset).
 *
 * Removes all parameter entries from NVS. Does NOT clear role config.
 *
 * @return 0 on success, negative errno on failure
 */
int rb_param_reset_all(void);

/**
 * @brief Check if parameter requires reboot to take effect.
 *
 * @param id Parameter identifier
 * @return true if reboot required, false otherwise
 */
bool rb_param_requires_reboot(enum rb_param_id id);

/**
 * @brief Get parameter descriptor by ID.
 *
 * @param id Parameter identifier
 * @return Pointer to descriptor, or NULL if id is invalid
 */
const struct rb_param_descriptor *rb_param_get_descriptor(enum rb_param_id id);

/**
 * @brief Check if parameter is persisted in NVS.
 *
 * @param id Parameter identifier
 * @return true if value is stored in NVS, false if using default
 */
bool rb_param_is_persisted(enum rb_param_id id);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_CONFIG_H_ */
