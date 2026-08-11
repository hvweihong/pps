#ifndef LINK_PROTOCOL_H_
#define LINK_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#define RB_ESB_MAX_PAYLOAD 252u
#define RB_PROTOCOL_MAGIC_0 ((uint8_t)'R')
#define RB_PROTOCOL_MAGIC_1 ((uint8_t)'B')
#define RB_PROTOCOL_VERSION 3u
#define RB_COMMON_HEADER_SIZE 10u
#define RB_SYNC_WIRE_SIZE 55u
#define RB_DATA_HEADER_SIZE 14u
#define RB_POLL_WIRE_SIZE 22u
#define RB_ACK_UPLINK_HEADER_SIZE 22u
#define RB_ACK_UPLINK_PAYLOAD_MAX \
	(RB_ESB_MAX_PAYLOAD - RB_ACK_UPLINK_HEADER_SIZE)
#define RB_PACKET_DATA_MAX (RB_ESB_MAX_PAYLOAD - RB_DATA_HEADER_SIZE)
#define RB_MAX_SOURCE_NODE 3u

_Static_assert(RB_SYNC_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "SYNC wire frame exceeds ESB payload limit");
_Static_assert(RB_POLL_WIRE_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "POLL wire frame exceeds ESB payload limit");
_Static_assert(RB_ACK_UPLINK_HEADER_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "ACK_UPLINK header exceeds ESB payload limit");
_Static_assert(RB_DATA_HEADER_SIZE <= RB_ESB_MAX_PAYLOAD,
	       "DOWNLINK_DATA header exceeds ESB payload limit");

enum rb_frame_type {
	RB_FRAME_SYNC = 1,
	RB_FRAME_DOWNLINK_DATA = 2,
	RB_FRAME_POLL = 3,
	RB_FRAME_ACK_UPLINK = 4,
};

enum rb_time_quality {
	RB_TIME_UTC_INVALID = 0,
	RB_TIME_LOCKED = 1,
	RB_TIME_HOLDOVER = 2,
};

struct rb_common_header {
	uint8_t type;
	uint32_t master_session;
	uint8_t source_node;
	uint8_t flags;
};

#define RB_COMMON_INIT(_type, _session, _source) \
	((struct rb_common_header){ \
		.type = (_type), .master_session = (_session), \
		.source_node = (_source), .flags = 0u })

struct rb_sync_frame {
	struct rb_common_header common;
	uint32_t group_id;
	uint64_t master_id;
	uint32_t sync_sequence;
	uint64_t previous_master_address_tick;
	uint64_t next_pps_master_tick;
	uint32_t sync_interval_us;
	int64_t next_pps_utc_seconds;
	uint8_t time_quality;
};

struct rb_downlink_frame {
	struct rb_common_header common;
	uint32_t sequence;
	const uint8_t *payload;
	size_t payload_len;
};

struct rb_poll {
	struct rb_common_header common;
	uint32_t known_slave_session;
	uint32_t uplink_ack_sequence;
	uint16_t next_credit_bytes;
	uint16_t poll_sequence;
};

struct rb_ack_uplink {
	struct rb_common_header common;
	uint32_t slave_session;
	uint32_t uplink_sequence;
	uint32_t drop_count;
	const uint8_t *payload;
	size_t payload_len;
};

int rb_common_encode(const struct rb_common_header *header,
		     uint8_t *wire, size_t wire_size);
int rb_common_decode(const uint8_t *wire, size_t wire_len,
		     struct rb_common_header *header);
int rb_sync_encode(const struct rb_sync_frame *frame,
		   uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_sync_decode(const uint8_t *wire, size_t wire_len,
		   struct rb_sync_frame *frame);
int rb_downlink_encode(uint32_t master_session, uint32_t sequence,
		       const uint8_t *payload, size_t payload_len,
		       uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_downlink_decode(const uint8_t *wire, size_t wire_len,
		       struct rb_downlink_frame *frame);
int rb_poll_encode(const struct rb_poll *frame,
		   uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_poll_decode(const uint8_t *wire, size_t wire_len,
		   struct rb_poll *frame);
int rb_ack_uplink_encode(const struct rb_ack_uplink *frame,
			 uint8_t *wire, size_t wire_size, size_t *wire_len);
int rb_ack_uplink_decode(const uint8_t *wire, size_t wire_len,
			 struct rb_ack_uplink *frame);

#endif /* LINK_PROTOCOL_H_ */
