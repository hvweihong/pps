#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static int cmd_boot_test_watchdog(const struct shell *shell, size_t argc,
				  char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_warn(shell, "boot_test watchdog armed; reset expected within 30 seconds");
	k_sleep(K_MSEC(100));
	(void)irq_lock();
	for (;;) {
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_boot_test,
	SHELL_CMD(watchdog, NULL, "Stop all interrupts and trigger the watchdog",
		  cmd_boot_test_watchdog),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(boot_test, &sub_boot_test,
		   "Validation-only boot recovery commands", NULL);
