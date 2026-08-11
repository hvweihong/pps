#include "boot_recovery.h"

enum rb_boot_recovery_action rb_boot_recovery_classify(
	uint32_t reset_reason, uint32_t watchdog_mask, bool retained_radio_valid)
{
	if ((reset_reason & watchdog_mask) == 0u) {
		return RB_BOOT_RECOVERY_NORMAL;
	}

	return retained_radio_valid ? RB_BOOT_RECOVERY_WATCHDOG_RADIO :
		RB_BOOT_RECOVERY_WATCHDOG;
}
