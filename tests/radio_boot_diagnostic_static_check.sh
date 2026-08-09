#!/usr/bin/env bash
set -euo pipefail

main=src/main.c
transport=src/radio_transport.c
header=src/radio_transport.h

rg -q 'RB_RADIO_BOOT_GUARD_KEY' "$main"
rg -q 'settings_load_subtree_direct\(RB_RADIO_BOOT_GUARD_KEY' "$main"
rg -q 'settings_save_one\(RB_RADIO_BOOT_GUARD_KEY' "$main"
rg -q 'settings_delete\(RB_RADIO_BOOT_GUARD_KEY\)' "$main"
rg -q 'radio boot guard recovered: reset_reason=' "$main"
rg -q 'retained=%u stage=' "$main"
rg -q '__noinit' "$transport"
for field in stage switch_index current_profile target_profile radio_state \
	             events_disabled hfclkstat last_result runtime_action \
	             radio_event_id payload_length primask ipsr; do
	rg -q "$field" "$header"
done
rg -q 'radio_transport_retained_diag_get' "$main" "$transport" "$header"
rg -q 'RB_RADIO_RETAINED_HISTORY_SIZE' "$header"
rg -q 'history_count' "$header" "$transport" "$main"
rg -q 'radio diag history:' "$main"
rg -q 'RB_RADIO_BOOT_STAGE_SEND' "$header" "$transport"
rg -q 'RB_RADIO_BOOT_STAGE_EVENT_HANDLER' "$header" "$transport"
rg -q 'RB_RADIO_BOOT_STAGE_ACTION' "$header" src/bridge_runtime.c
rg -q 'radio_transport_retained_diag_note' src/bridge_runtime.c "$transport" "$header"
! rg -q 'settings_save_one|settings_delete' "$transport"
! rg -q 'RADIO_BOOT_STAGE_REG|gpregret_set\(NRF_POWER, RADIO_BOOT_STAGE_REG' \
	"$main" "$transport"

echo "radio boot diagnostic static check: PASS"
