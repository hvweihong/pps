#ifndef BOOT_RECOVERY_H_
#define BOOT_RECOVERY_H_

#include <stdbool.h>
#include <stdint.h>

enum rb_boot_recovery_action {
	RB_BOOT_RECOVERY_NORMAL,
	RB_BOOT_RECOVERY_WATCHDOG,
	RB_BOOT_RECOVERY_WATCHDOG_RADIO,
};

enum rb_boot_recovery_action rb_boot_recovery_classify(
	uint32_t reset_reason, uint32_t watchdog_mask, bool retained_radio_valid);

#endif
