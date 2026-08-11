#include "status.h"

#include <zephyr/logging/log.h>

#include "bridge_runtime.h"
#include "pps_output.h"
#include "uart_bridge.h"
#include "utc_clock.h"

LOG_MODULE_REGISTER(star_bridge_status, LOG_LEVEL_INF);

static const char *utc_quality_name(uint8_t quality)
{
	switch (quality) {
	case RB_TIME_LOCKED:
		return "LOCKED";
	case RB_TIME_HOLDOVER:
		return "HOLDOVER";
	default:
		return "UTC_INVALID";
	}
}

static const char *time_source_name(const struct rb_bridge_stats *bridge)
{
	if (!bridge->is_master) {
		return "WIRELESS";
	}
	if (bridge->time_source_mode == 0u) {
		return "LOCAL";
	}
	switch (bridge->utc_state) {
	case RB_UTC_LOCKED:
		return "EXTERNAL_LOCKED";
	case RB_UTC_HOLDOVER:
		return "EXTERNAL_HOLDOVER";
	case RB_UTC_INVALID:
		return "EXTERNAL_INVALID";
	default:
		return "EXTERNAL_ACQUIRING";
	}
}

void status_log_bridge(void)
{
	struct rb_bridge_stats bridge;
	const struct rb_uart_stats *uart = uart_bridge_stats_get();
	const struct pps_output_stats *pps = pps_output_stats_get();

	bridge_runtime_stats_get(&bridge);
	LOG_INF("bridge_status role=%s time_source=%s utc_quality=%s "
		"utc_seconds=%lld uart_rx=%llu uart_tx=%llu uart_drop=%llu/%llu "
		"pps=%u sync_state=%u sync_age_us=%u",
		bridge.is_master ? "master" : "slave",
		time_source_name(&bridge), utc_quality_name(bridge.utc_quality),
		(long long)bridge.utc_seconds,
		(unsigned long long)uart->rx_bytes,
		(unsigned long long)uart->tx_bytes,
		(unsigned long long)uart->rx_drop_bytes,
		(unsigned long long)uart->tx_drop_bytes,
		pps->pulses, bridge.sync_state, bridge.sync_age_us);
	LOG_INF("bridge_link active_count=%u active_mask=0x%02x "
		"node1_ack_age_us=%lld node1_poll_failures=%u "
		"node2_ack_age_us=%lld node2_poll_failures=%u "
		"node3_ack_age_us=%lld node3_poll_failures=%u "
		"slave_active=%u slave_node_id=%u slave_session=%u "
		"invalid_session_packets=%llu invalid_node_packets=%llu "
		"invalid_group_packets=%llu duplicate_packets=%llu "
		"downlink_gap_packets=%llu queue_drop_bytes=%llu action_errors=%llu",
		bridge.active_count, bridge.active_mask,
		(long long)bridge.node1_ack_age_us, bridge.node1_poll_failures,
		(long long)bridge.node2_ack_age_us, bridge.node2_poll_failures,
		(long long)bridge.node3_ack_age_us, bridge.node3_poll_failures,
		bridge.slave_active, bridge.slave_node_id, bridge.slave_session,
		(unsigned long long)bridge.invalid_session_packets,
		(unsigned long long)bridge.invalid_node_packets,
		(unsigned long long)bridge.invalid_group_packets,
		(unsigned long long)bridge.duplicate_packets,
		(unsigned long long)bridge.downlink_gap_packets,
		(unsigned long long)bridge.queue_drop_bytes,
		(unsigned long long)bridge.action_error_count);
}
