#ifndef LINK_PROTOCOL_H_
#define LINK_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#define RB_ESB_MAX_PAYLOAD 252u
#define RB_PROTOCOL_MAGIC_0 ((uint8_t)'R')
#define RB_PROTOCOL_MAGIC_1 ((uint8_t)'B')
#define RB_PROTOCOL_VERSION 2u
#define RB_COMMON_HEADER_SIZE 12u
#define RB_SYNC_DISCOVERY_WIRE_SIZE 65u
#define RB_DATA_HEADER_SIZE 18u
#define RB_HELLO_WIRE_SIZE 28u
#define RB_ASSIGN_WIRE_SIZE 34u
#define RB_POLL_WIRE_SIZE 30u
#define RB_ACK_UPLINK_HEADER_SIZE 36u
#define RB_ACK_UPLINK_PAYLOAD_MAX \
	(RB_ESB_MAX_PAYLOAD - RB_ACK_UPLINK_HEADER_SIZE)
#define RB_SKIP_TO_WIRE_SIZE 23u
#define RB_MAX_SOURCE_NODE 3u

/* Compile-time guards: every frame type's maximum wire size must fit within
 * the ESB 252-byte payload limit.  A DATA/ACK frame can carry payload bytes
 * on top of its fixed header, so those are checked against the header alone;
 * RB_PACKET_DATA_MAX in link_window.h caps DATA payloads, while
 * RB_ACK_UPLINK_PAYLOAD_MAX caps the larger ACK_UPLINK frame. */
_Static_assert(RB_SYNC_DISCOVERY_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "SYNC_DISCOVERY wire frame exceeds ESB payload limit");
_Static_assert(RB_HELLO_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "HELLO wire frame exceeds ESB payload limit");
_Static_assert(RB_ASSIGN_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "ASSIGN wire frame exceeds ESB payload limit");
_Static_assert(RB_POLL_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "POLL wire frame exceeds ESB payload limit");
_Static_assert(RB_ACK_UPLINK_HEADER_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "ACK_UPLINK header exceeds ESB payload limit");
_Static_assert(RB_SKIP_TO_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "SKIP_TO wire frame exceeds ESB payload limit");
_Static_assert(RB_DATA_HEADER_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "DATA header exceeds ESB payload limit");

enum rb_frame_type {
	RB_FRAME_SYNC_DISCOVERY = 1,
	RB_FRAME_HELLO = 2,
	RB_FRAME_ASSIGN = 3,
	RB_FRAME_DOWNLINK_DATA = 4,
	RB_FRAME_REPAIR_DATA = 5,
	RB_FRAME_POLL = 6,
	RB_FRAME_ACK_UPLINK = 7,
	RB_FRAME_SKIP_TO = 8,
};

enum rb_time_quality {
	RB_TIME_UTC_INVALID = 0,
	RB_TIME_LOCKED = 1,
	RB_TIME_HOLDOVER = 2,
};

struct rb_common_header {
	uint8_t type;
	uint32_t master_session;
	uint16_t lease_id;
	uint8_t source_node;
	uint8_t flags;
};

#define RB_COMMON_INIT(_type, _session, _lease, _source) \
	((struct rb_common_header){ \
		.type = (_type), .master_session = (_session), \
		.lease_id = (_lease), .source_node = (_source), .flags = 0 })

struct rb_sync_discovery {
	struct rb_common_header common;
	uint32_t group_id;
	uint64_t master_id;
	uint32_t sync_sequence;
	uint64_t previous_master_address_tick;
	uint64_t next_pps_master_tick;
	uint32_t sync_interval_us;
	uint32_t discovery_nonce;
	uint8_t free_slots;
	uint8_t response_slot_count;
	uint16_t response_slot_us;
	int64_t next_pps_utc_seconds;
	uint8_t time_quality;
};

struct rb_data_frame {
	struct rb_common_header common;
	uint16_t stream_epoch;
	uint32_t sequence;
	const uint8_t *payload;
	size_t payload_len;
};

struct rb_hello {
	struct rb_common_header common;
	uint64_t device_id;
	uint32_t discovery_nonce;
	uint32_t capabilities;
};

struct rb_assign {
	struct rb_common_header common;
	uint64_t target_device_id;
	uint8_t node_id;
	uint8_t pipe;
	uint16_t downlink_epoch;
	uint16_t uplink_epoch;
	uint16_t max_payload;
	uint8_t link_window;
	uint8_t retry_count;
	uint32_t lease_timeout_us;
};

struct rb_poll {
	struct rb_common_header common;
	uint16_t uplink_epoch;
	uint32_t uplink_ack_base;
	uint64_t uplink_ack_bitmap;
	uint16_t next_credit_bytes;
	uint16_t poll_sequence;
};

struct rb_ack_uplink {
	struct rb_common_header common;
	uint16_t uplink_epoch;
	uint32_t uplink_sequence;
	uint16_t downlink_epoch;
	uint32_t downlink_ack_base;
	uint64_t downlink_ack_bitmap;
	uint32_t drop_count;
	const uint8_t *payload;
	size_t payload_len;
};

struct rb_skip_to {
	struct rb_common_header common;
	uint8_t direction;
	uint16_t stream_epoch;
	uint32_t next_sequence;
	uint32_t drop_count;
};

int rb_common_encode(const struct rb_common_header *header,
			     uint8_t *wire, size_t wire_size);
int rb_common_decode(const uint8_t *wire, size_t wire_len,
			     struct rb_common_header *header);
int rb_sync_discovery_encode(const struct rb_sync_discovery *frame,
				     uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_sync_discovery_decode(const uint8_t *wire, size_t wire_len,
				     struct rb_sync_discovery *frame);
int rb_data_encode(uint8_t type, uint32_t master_session, uint16_t lease_id,
			   uint16_t stream_epoch, uint32_t sequence,
			   const uint8_t *payload, size_t payload_len,
			   uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_data_decode(const uint8_t *wire, size_t wire_len,
			  struct rb_data_frame *frame);
int rb_hello_encode(const struct rb_hello *frame,
			uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_hello_decode(const uint8_t *wire, size_t wire_len,
			struct rb_hello *frame);
int rb_assign_encode(const struct rb_assign *frame,
			 uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_assign_decode(const uint8_t *wire, size_t wire_len,
			 struct rb_assign *frame);
int rb_poll_encode(const struct rb_poll *frame,
		       uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_poll_decode(const uint8_t *wire, size_t wire_len,
		       struct rb_poll *frame);
int rb_ack_uplink_encode(const struct rb_ack_uplink *frame,
			     uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_ack_uplink_decode(const uint8_t *wire, size_t wire_len,
			     struct rb_ack_uplink *frame);
int rb_skip_to_encode(const struct rb_skip_to *frame,
			  uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_skip_to_decode(const uint8_t *wire, size_t wire_len,
			  struct rb_skip_to *frame);

#endif /* LINK_PROTOCOL_H_ */
