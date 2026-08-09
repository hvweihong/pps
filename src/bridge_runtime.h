#ifndef BRIDGE_RUNTIME_H_
#define BRIDGE_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>
#include "link_protocol.h"
#include "sync_filter.h"

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
#include "bridge_validation.h"
#endif

struct rb_bridge_stats {
	uint64_t radio_tx_packets;
	uint64_t radio_rx_packets;
	uint64_t radio_retry_count;
	uint64_t radio_retry_exhausted;
	uint64_t broadcast_packets;
	uint64_t poll_packets;
	uint64_t duplicate_packets;
	uint64_t downlink_gap_packets;
	uint64_t downlink_duplicate_packets;
	uint64_t invalid_session_packets;
	uint64_t queue_drop_bytes;
	uint64_t node_record_count[RB_MAX_SOURCE_NODE];
	uint64_t node_record_bytes[RB_MAX_SOURCE_NODE];
	uint64_t node_record_drop[RB_MAX_SOURCE_NODE];
	uint64_t discovery_hello_count;
	uint64_t sync_rx_count;
	uint64_t sync_missed_count;
	uint64_t sync_pair_count;
	uint64_t sync_tracker_wait_count;
	uint64_t sync_tracker_error_count;
	uint64_t sync_filter_update_count;
	uint64_t sync_filter_error_count;
	uint64_t sync_pps_reset_error_count;
	uint64_t sync_tx_build_count;
	uint64_t sync_tx_capture_count;
	uint64_t sync_tx_failure_count;
	uint64_t sync_tx_previous_count;
	uint64_t sync_last_tx_tick;
	uint64_t sync_last_previous_master_tick;
	uint64_t sync_relock_count;
	uint64_t external_pps_count;
	uint64_t nmea_valid_count;
	uint64_t nmea_drop_count;
	uint64_t holdover_count;
	uint64_t action_error_count;
	int64_t utc_seconds;
	int64_t sync_offset;
	uint32_t sync_age_us;
	uint32_t sync_jitter;
	uint32_t sync_last_sequence;
	uint32_t sync_last_tx_sequence;
	int32_t sync_last_error;
	int32_t sync_last_pps_reset_error;
	int32_t last_action_error;
	uint8_t active_count;
	uint8_t suspect_count;
	uint8_t sync_state;
	uint8_t slave_active;
	uint8_t slave_node_id;
	uint8_t last_action;
	uint8_t last_action_error_action;
	uint8_t is_master;
	uint8_t time_source_mode;
	uint8_t utc_state;
	uint8_t utc_quality;
};

int bridge_runtime_init(void);
int bridge_runtime_start(void);
void bridge_runtime_stats_get(struct rb_bridge_stats *stats);
enum sync_filter_state bridge_runtime_sync_state(void);
int64_t bridge_runtime_sync_offset_us(void);
int32_t bridge_runtime_sync_drift_ppm(void);

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
int bridge_runtime_validation_inject(size_t len, uint8_t seed);
int bridge_runtime_validation_verify(size_t len, uint8_t seed,
				     size_t *mismatch_offset);
size_t bridge_runtime_validation_copy(size_t offset, uint8_t *data,
				      size_t max_len);
void bridge_runtime_validation_clear(void);
void bridge_runtime_validation_stats_get(struct rb_validation_stats *stats);
int bridge_runtime_validation_time_pair(int64_t utc_seconds);
int bridge_runtime_validation_time_source_lost(void);
#endif

#endif /* BRIDGE_RUNTIME_H_ */
