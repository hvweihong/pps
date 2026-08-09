#!/usr/bin/env bash
set -euo pipefail

rg -q '^CONFIG_WATCHDOG=y$' prj.conf
rg -q '^CONFIG_WDT_NRFX=y$' prj.conf
rg -q 'DFU_MAGIC_UF2_RESET' src/main.c
rg -q 'usbd_msg_register_cb' src/main.c
rg -q 'wdt_feed' src/main.c
! rg -q 'DFU_MAGIC_UF2_RESET|usbd_msg_register_cb|usbd_line_coding_cb' \
	src/bridge_runtime.c
