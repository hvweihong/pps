#!/usr/bin/env bash
set -euo pipefail

src=src/radio_transport.c
prj=prj.conf
for token in CONFIG_ESB_DYNAMIC_INTERRUPTS CONFIG_ESB_MPSL_TIMESLOT ESB_PROTOCOL_ESB_DPL ESB_BITRATE_2MBPS ESB_CRC_16BIT CONFIG_ESB_MAX_PAYLOAD_LENGTH CONFIG_ESB_SYS_TIMER1; do
    rg -q "$token" "$src" "$prj"
done
rg -q 'esb_disable' "$src"
rg -q 'esb_set_rf_channel' "$src"
rg -q 'k_msgq' "$src"
rg -q 'TIMEBASE_RADIO_CAPTURE_CHANNEL|timebase_capture64\(3\)' "$src"
for token in NRF_RADIO_EVENT_ADDRESS timebase_timer_capture_task_address \
             nrfx_gppi_conn_alloc nrfx_gppi_conn_enable radio_capture_init; do
    rg -q "$token" "$src"
done
rg -Fq 'LOG_DBG("ESB configured:' "$src"
if rg -Fq 'LOG_INF("ESB configured:' "$src"; then
    echo "normal ESB profile switches must not flood INFO logs" >&2
    exit 1
fi

# ASSIGN must be sent to the candidate's temporary DEVICEID-derived address, not
# the stable group address, or the hardware ACK required for admission never
# arrives. See docs/handoff for the incident this check guards against.
if ! rg -UPq 'temporary_address != NULL &&\s*\n\s*\(profile == RB_RADIO_SLAVE_HELLO_PTX \|\|\s*\n\s*profile == RB_RADIO_SLAVE_ASSIGN_PRX \|\|\s*\n\s*profile == RB_RADIO_MASTER_ASSIGN_PTX\)' "$src"; then
    echo "radio_transport_set_profile must install the temporary address for RB_RADIO_MASTER_ASSIGN_PTX" >&2
    exit 1
fi
echo "radio transport static check: PASS"
