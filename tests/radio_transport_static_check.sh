#!/usr/bin/env bash
set -euo pipefail

src=src/radio_transport.c
prj=prj.conf
scheduler=src/link_scheduler_core.c
runtime=src/bridge_runtime.c
protocol=src/link_protocol.h
validation_shell=src/bridge_validation_shell.c
loss_conf=loss-validation.conf
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

for removed in RB_ACTION_SEND_REPAIR RB_ACTION_SEND_SKIP_TO \
               rb_scheduler_note_downlink_ack rb_scheduler_take_evicted \
               downlink_history downlink_ack_bitmap; do
    if rg -q "$removed" "$scheduler" "$runtime" "$protocol"; then
        echo "removed downlink reliability token remains: $removed" >&2
        exit 1
    fi
done
rg -q '^#define RB_ACK_UPLINK_HEADER_SIZE 22u$' "$protocol"
if ! rg -UPq 'case RB_ACTION_SEND_DOWNLINK_BROADCAST:[\s\S]*?radio_transport_send\(action->pipe, true,[\s\S]*?case RB_ACTION_SEND_SYNC_DISCOVERY:' "$runtime"; then
    echo "business downlink must explicitly use no-ACK radio send" >&2
    exit 1
fi
rg -q '^CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION=y$' "$loss_conf"
rg -q 'SHELL_CMD_ARG\(loss,' "$validation_shell"
rg -q 'SHELL_CMD\(loss_off,' "$validation_shell"
rg -q 'radio_transport_loss_set' "$validation_shell"
rg -q 'queue_injected_tx_success\(\)' "$src"
if ! rg -UPq 'if \(loss_type_mask != 0u &&\s*\n\s*\(loss_type_mask & \(1u << frame_type\)\) == 0u\) \{\s*\n\s*return false;' "$src"; then
    echo "loss cadence must count only matching frame types" >&2
    exit 1
fi
echo "radio transport static check: PASS"
