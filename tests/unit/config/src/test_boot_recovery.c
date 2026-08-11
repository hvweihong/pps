#include <zephyr/ztest.h>

#include "boot_recovery.h"

#define TEST_DOG_MASK (1u << 1)

ZTEST(boot_recovery, test_non_watchdog_reset_ignores_stale_retained_data)
{
	zassert_equal(rb_boot_recovery_classify(0u, TEST_DOG_MASK, true),
		      RB_BOOT_RECOVERY_NORMAL);
}

ZTEST(boot_recovery, test_watchdog_without_radio_context_is_generic)
{
	zassert_equal(rb_boot_recovery_classify(TEST_DOG_MASK, TEST_DOG_MASK, false),
		      RB_BOOT_RECOVERY_WATCHDOG);
}

ZTEST(boot_recovery, test_watchdog_with_radio_context_reports_radio)
{
	zassert_equal(rb_boot_recovery_classify(TEST_DOG_MASK, TEST_DOG_MASK, true),
		      RB_BOOT_RECOVERY_WATCHDOG_RADIO);
}

ZTEST_SUITE(boot_recovery, NULL, NULL, NULL, NULL, NULL);
