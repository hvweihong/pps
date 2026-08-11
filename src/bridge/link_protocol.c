#include "link_protocol.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define RB_COMMON_SESSION_OFFSET 4u
#define RB_COMMON_SOURCE_OFFSET 8u
#define RB_COMMON_FLAGS_OFFSET 9u

#define RB_SYNC_GROUP_OFFSET RB_COMMON_HEADER_SIZE
#define RB_SYNC_MASTER_ID_OFFSET (RB_SYNC_GROUP_OFFSET + 4u)
#define RB_SYNC_SEQUENCE_OFFSET (RB_SYNC_MASTER_ID_OFFSET + 8u)
#define RB_SYNC_PREVIOUS_TICK_OFFSET (RB_SYNC_SEQUENCE_OFFSET + 4u)
#define RB_SYNC_NEXT_PPS_OFFSET (RB_SYNC_PREVIOUS_TICK_OFFSET + 8u)
#define RB_SYNC_INTERVAL_OFFSET (RB_SYNC_NEXT_PPS_OFFSET + 8u)
#define RB_SYNC_UTC_OFFSET (RB_SYNC_INTERVAL_OFFSET + 4u)
#define RB_SYNC_QUALITY_OFFSET (RB_SYNC_UTC_OFFSET + 8u)

#define RB_DATA_SEQUENCE_OFFSET RB_COMMON_HEADER_SIZE

#define RB_POLL_SLAVE_SESSION_OFFSET RB_COMMON_HEADER_SIZE
#define RB_POLL_ACK_SEQUENCE_OFFSET (RB_POLL_SLAVE_SESSION_OFFSET + 4u)
#define RB_POLL_CREDIT_OFFSET (RB_POLL_ACK_SEQUENCE_OFFSET + 4u)
#define RB_POLL_SEQUENCE_OFFSET (RB_POLL_CREDIT_OFFSET + 2u)

#define RB_ACK_SLAVE_SESSION_OFFSET RB_COMMON_HEADER_SIZE
#define RB_ACK_SEQUENCE_OFFSET (RB_ACK_SLAVE_SESSION_OFFSET + 4u)
#define RB_ACK_DROP_OFFSET (RB_ACK_SEQUENCE_OFFSET + 4u)

static bool frame_type_valid(uint8_t type)
{
	return type >= RB_FRAME_SYNC && type <= RB_FRAME_ACK_UPLINK;
}

static bool source_valid(uint8_t source_node)
{
	return source_node <= RB_MAX_SOURCE_NODE;
}

static bool quality_valid(uint8_t quality)
{
	return quality <= RB_TIME_HOLDOVER;
}

int rb_common_encode(const struct rb_common_header *header,
		     uint8_t *wire, size_t wire_size)
{
	if (header == NULL || wire == NULL || wire_size < RB_COMMON_HEADER_SIZE ||
	    !frame_type_valid(header->type) || header->master_session == 0u ||
	    !source_valid(header->source_node)) {
		return -EINVAL;
	}
	wire[0] = RB_PROTOCOL_MAGIC_0;
	wire[1] = RB_PROTOCOL_MAGIC_1;
	wire[2] = RB_PROTOCOL_VERSION;
	wire[3] = header->type;
	sys_put_le32(header->master_session, &wire[RB_COMMON_SESSION_OFFSET]);
	wire[RB_COMMON_SOURCE_OFFSET] = header->source_node;
	wire[RB_COMMON_FLAGS_OFFSET] = header->flags;
	return 0;
}

int rb_common_decode(const uint8_t *wire, size_t wire_len,
		     struct rb_common_header *header)
{
	if (wire == NULL || header == NULL || wire_len < RB_COMMON_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	if (wire[0] != RB_PROTOCOL_MAGIC_0 || wire[1] != RB_PROTOCOL_MAGIC_1 ||
	    wire[2] != RB_PROTOCOL_VERSION) {
		return -EBADMSG;
	}
	header->type = wire[3];
	header->master_session = sys_get_le32(&wire[RB_COMMON_SESSION_OFFSET]);
	header->source_node = wire[RB_COMMON_SOURCE_OFFSET];
	header->flags = wire[RB_COMMON_FLAGS_OFFSET];
	if (!frame_type_valid(header->type) || header->master_session == 0u ||
	    !source_valid(header->source_node)) {
		return -EINVAL;
	}
	return 0;
}

int rb_sync_encode(const struct rb_sync_frame *frame,
		   uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || wire == NULL || wire_len == NULL ||
	    wire_size < RB_SYNC_WIRE_SIZE || frame->common.type != RB_FRAME_SYNC ||
	    frame->common.source_node != 0u || frame->group_id == 0u ||
	    frame->group_id == UINT32_MAX || frame->master_id == 0u ||
	    frame->sync_sequence == 0u || frame->sync_interval_us == 0u ||
	    !quality_valid(frame->time_quality)) {
		return -EINVAL;
	}
	ret = rb_common_encode(&frame->common, wire, wire_size);
	if (ret != 0) {
		return ret;
	}
	sys_put_le32(frame->group_id, &wire[RB_SYNC_GROUP_OFFSET]);
	sys_put_le64(frame->master_id, &wire[RB_SYNC_MASTER_ID_OFFSET]);
	sys_put_le32(frame->sync_sequence, &wire[RB_SYNC_SEQUENCE_OFFSET]);
	sys_put_le64(frame->previous_master_address_tick,
		     &wire[RB_SYNC_PREVIOUS_TICK_OFFSET]);
	sys_put_le64(frame->next_pps_master_tick, &wire[RB_SYNC_NEXT_PPS_OFFSET]);
	sys_put_le32(frame->sync_interval_us, &wire[RB_SYNC_INTERVAL_OFFSET]);
	sys_put_le64((uint64_t)frame->next_pps_utc_seconds,
		     &wire[RB_SYNC_UTC_OFFSET]);
	wire[RB_SYNC_QUALITY_OFFSET] = frame->time_quality;
	*wire_len = RB_SYNC_WIRE_SIZE;
	return 0;
}

int rb_sync_decode(const uint8_t *wire, size_t wire_len,
		   struct rb_sync_frame *frame)
{
	struct rb_sync_frame decoded;
	int ret;

	if (wire == NULL || frame == NULL || wire_len != RB_SYNC_WIRE_SIZE) {
		return -EMSGSIZE;
	}
	memset(&decoded, 0, sizeof(decoded));
	ret = rb_common_decode(wire, wire_len, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	decoded.group_id = sys_get_le32(&wire[RB_SYNC_GROUP_OFFSET]);
	decoded.master_id = sys_get_le64(&wire[RB_SYNC_MASTER_ID_OFFSET]);
	decoded.sync_sequence = sys_get_le32(&wire[RB_SYNC_SEQUENCE_OFFSET]);
	decoded.previous_master_address_tick =
		sys_get_le64(&wire[RB_SYNC_PREVIOUS_TICK_OFFSET]);
	decoded.next_pps_master_tick = sys_get_le64(&wire[RB_SYNC_NEXT_PPS_OFFSET]);
	decoded.sync_interval_us = sys_get_le32(&wire[RB_SYNC_INTERVAL_OFFSET]);
	decoded.next_pps_utc_seconds =
		(int64_t)sys_get_le64(&wire[RB_SYNC_UTC_OFFSET]);
	decoded.time_quality = wire[RB_SYNC_QUALITY_OFFSET];
	if (decoded.common.type != RB_FRAME_SYNC ||
	    decoded.common.source_node != 0u || decoded.group_id == 0u ||
	    decoded.group_id == UINT32_MAX || decoded.master_id == 0u ||
	    decoded.sync_sequence == 0u || decoded.sync_interval_us == 0u ||
	    !quality_valid(decoded.time_quality)) {
		return -EINVAL;
	}
	*frame = decoded;
	return 0;
}

int rb_downlink_encode(uint32_t master_session, uint32_t sequence,
		       const uint8_t *payload, size_t payload_len,
		       uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	struct rb_common_header common =
		RB_COMMON_INIT(RB_FRAME_DOWNLINK_DATA, master_session, 0u);
	int ret;

	if (wire == NULL || wire_len == NULL || payload == NULL || payload_len == 0u ||
	    payload_len > RB_PACKET_DATA_MAX || wire_size < RB_DATA_HEADER_SIZE + payload_len ||
	    sequence == 0u) {
		return -EINVAL;
	}
	ret = rb_common_encode(&common, wire, wire_size);
	if (ret != 0) {
		return ret;
	}
	sys_put_le32(sequence, &wire[RB_DATA_SEQUENCE_OFFSET]);
	memcpy(&wire[RB_DATA_HEADER_SIZE], payload, payload_len);
	*wire_len = RB_DATA_HEADER_SIZE + payload_len;
	return 0;
}

int rb_downlink_decode(const uint8_t *wire, size_t wire_len,
		       struct rb_downlink_frame *frame)
{
	struct rb_downlink_frame decoded;
	int ret;

	if (wire == NULL || frame == NULL || wire_len <= RB_DATA_HEADER_SIZE ||
	    wire_len > RB_ESB_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}
	memset(&decoded, 0, sizeof(decoded));
	ret = rb_common_decode(wire, wire_len, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	decoded.sequence = sys_get_le32(&wire[RB_DATA_SEQUENCE_OFFSET]);
	decoded.payload = &wire[RB_DATA_HEADER_SIZE];
	decoded.payload_len = wire_len - RB_DATA_HEADER_SIZE;
	if (decoded.common.type != RB_FRAME_DOWNLINK_DATA ||
	    decoded.common.source_node != 0u || decoded.sequence == 0u) {
		return -EINVAL;
	}
	*frame = decoded;
	return 0;
}

int rb_poll_encode(const struct rb_poll *frame,
		   uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || wire == NULL || wire_len == NULL ||
	    wire_size < RB_POLL_WIRE_SIZE || frame->common.type != RB_FRAME_POLL ||
	    frame->common.source_node != 0u ||
	    frame->next_credit_bytes > RB_ACK_UPLINK_PAYLOAD_MAX) {
		return -EINVAL;
	}
	ret = rb_common_encode(&frame->common, wire, wire_size);
	if (ret != 0) {
		return ret;
	}
	sys_put_le32(frame->known_slave_session,
		     &wire[RB_POLL_SLAVE_SESSION_OFFSET]);
	sys_put_le32(frame->uplink_ack_sequence,
		     &wire[RB_POLL_ACK_SEQUENCE_OFFSET]);
	sys_put_le16(frame->next_credit_bytes, &wire[RB_POLL_CREDIT_OFFSET]);
	sys_put_le16(frame->poll_sequence, &wire[RB_POLL_SEQUENCE_OFFSET]);
	*wire_len = RB_POLL_WIRE_SIZE;
	return 0;
}

int rb_poll_decode(const uint8_t *wire, size_t wire_len,
		   struct rb_poll *frame)
{
	struct rb_poll decoded;
	int ret;

	if (wire == NULL || frame == NULL || wire_len != RB_POLL_WIRE_SIZE) {
		return -EMSGSIZE;
	}
	memset(&decoded, 0, sizeof(decoded));
	ret = rb_common_decode(wire, wire_len, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	decoded.known_slave_session =
		sys_get_le32(&wire[RB_POLL_SLAVE_SESSION_OFFSET]);
	decoded.uplink_ack_sequence =
		sys_get_le32(&wire[RB_POLL_ACK_SEQUENCE_OFFSET]);
	decoded.next_credit_bytes = sys_get_le16(&wire[RB_POLL_CREDIT_OFFSET]);
	decoded.poll_sequence = sys_get_le16(&wire[RB_POLL_SEQUENCE_OFFSET]);
	if (decoded.common.type != RB_FRAME_POLL ||
	    decoded.common.source_node != 0u ||
	    decoded.next_credit_bytes > RB_ACK_UPLINK_PAYLOAD_MAX) {
		return -EINVAL;
	}
	*frame = decoded;
	return 0;
}

int rb_ack_uplink_encode(const struct rb_ack_uplink *frame,
			 uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || wire == NULL || wire_len == NULL ||
	    frame->common.type != RB_FRAME_ACK_UPLINK ||
	    frame->common.source_node == 0u ||
	    frame->common.source_node > RB_MAX_SOURCE_NODE ||
	    frame->slave_session == 0u || frame->payload_len > RB_ACK_UPLINK_PAYLOAD_MAX ||
	    (frame->payload_len != 0u &&
	     (frame->payload == NULL || frame->uplink_sequence == 0u)) ||
	    (frame->payload_len == 0u && frame->uplink_sequence != 0u) ||
	    wire_size < RB_ACK_UPLINK_HEADER_SIZE + frame->payload_len) {
		return -EINVAL;
	}
	ret = rb_common_encode(&frame->common, wire, wire_size);
	if (ret != 0) {
		return ret;
	}
	sys_put_le32(frame->slave_session, &wire[RB_ACK_SLAVE_SESSION_OFFSET]);
	sys_put_le32(frame->uplink_sequence, &wire[RB_ACK_SEQUENCE_OFFSET]);
	sys_put_le32(frame->drop_count, &wire[RB_ACK_DROP_OFFSET]);
	if (frame->payload_len != 0u) {
		memcpy(&wire[RB_ACK_UPLINK_HEADER_SIZE], frame->payload,
		       frame->payload_len);
	}
	*wire_len = RB_ACK_UPLINK_HEADER_SIZE + frame->payload_len;
	return 0;
}

int rb_ack_uplink_decode(const uint8_t *wire, size_t wire_len,
			 struct rb_ack_uplink *frame)
{
	struct rb_ack_uplink decoded;
	int ret;

	if (wire == NULL || frame == NULL || wire_len < RB_ACK_UPLINK_HEADER_SIZE ||
	    wire_len > RB_ESB_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}
	memset(&decoded, 0, sizeof(decoded));
	ret = rb_common_decode(wire, wire_len, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	decoded.slave_session = sys_get_le32(&wire[RB_ACK_SLAVE_SESSION_OFFSET]);
	decoded.uplink_sequence = sys_get_le32(&wire[RB_ACK_SEQUENCE_OFFSET]);
	decoded.drop_count = sys_get_le32(&wire[RB_ACK_DROP_OFFSET]);
	decoded.payload = wire_len == RB_ACK_UPLINK_HEADER_SIZE ? NULL :
		&wire[RB_ACK_UPLINK_HEADER_SIZE];
	decoded.payload_len = wire_len - RB_ACK_UPLINK_HEADER_SIZE;
	if (decoded.common.type != RB_FRAME_ACK_UPLINK ||
	    decoded.common.source_node == 0u ||
	    decoded.common.source_node > RB_MAX_SOURCE_NODE ||
	    decoded.slave_session == 0u ||
	    (decoded.payload_len == 0u && decoded.uplink_sequence != 0u) ||
	    (decoded.payload_len != 0u && decoded.uplink_sequence == 0u)) {
		return -EINVAL;
	}
	*frame = decoded;
	return 0;
}
