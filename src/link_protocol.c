#include "link_protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#define RB_COMMON_TYPE_OFFSET 3u
#define RB_COMMON_SESSION_OFFSET 4u
#define RB_COMMON_LEASE_OFFSET 8u
#define RB_COMMON_SOURCE_OFFSET 10u
#define RB_COMMON_FLAGS_OFFSET 11u

#define RB_SYNC_GROUP_OFFSET RB_COMMON_HEADER_SIZE
#define RB_SYNC_MASTER_ID_OFFSET (RB_SYNC_GROUP_OFFSET + 4u)
#define RB_SYNC_SEQUENCE_OFFSET (RB_SYNC_MASTER_ID_OFFSET + 8u)
#define RB_SYNC_PREVIOUS_TICK_OFFSET (RB_SYNC_SEQUENCE_OFFSET + 4u)
#define RB_SYNC_NEXT_PPS_OFFSET (RB_SYNC_PREVIOUS_TICK_OFFSET + 8u)
#define RB_SYNC_INTERVAL_OFFSET (RB_SYNC_NEXT_PPS_OFFSET + 8u)
#define RB_SYNC_NONCE_OFFSET (RB_SYNC_INTERVAL_OFFSET + 4u)
#define RB_SYNC_FREE_SLOTS_OFFSET (RB_SYNC_NONCE_OFFSET + 4u)
#define RB_SYNC_RESPONSE_COUNT_OFFSET (RB_SYNC_FREE_SLOTS_OFFSET + 1u)
#define RB_SYNC_RESPONSE_SLOT_US_OFFSET (RB_SYNC_RESPONSE_COUNT_OFFSET + 1u)

#define RB_DATA_EPOCH_OFFSET RB_COMMON_HEADER_SIZE
#define RB_DATA_SEQUENCE_OFFSET (RB_DATA_EPOCH_OFFSET + 2u)

#define RB_HELLO_DEVICE_ID_OFFSET RB_COMMON_HEADER_SIZE
#define RB_HELLO_NONCE_OFFSET (RB_HELLO_DEVICE_ID_OFFSET + 8u)
#define RB_HELLO_CAPABILITIES_OFFSET (RB_HELLO_NONCE_OFFSET + 4u)

#define RB_ASSIGN_TARGET_ID_OFFSET RB_COMMON_HEADER_SIZE
#define RB_ASSIGN_NODE_OFFSET (RB_ASSIGN_TARGET_ID_OFFSET + 8u)
#define RB_ASSIGN_PIPE_OFFSET (RB_ASSIGN_NODE_OFFSET + 1u)
#define RB_ASSIGN_DOWNLINK_EPOCH_OFFSET (RB_ASSIGN_PIPE_OFFSET + 1u)
#define RB_ASSIGN_UPLINK_EPOCH_OFFSET (RB_ASSIGN_DOWNLINK_EPOCH_OFFSET + 2u)
#define RB_ASSIGN_MAX_PAYLOAD_OFFSET (RB_ASSIGN_UPLINK_EPOCH_OFFSET + 2u)
#define RB_ASSIGN_LINK_WINDOW_OFFSET (RB_ASSIGN_MAX_PAYLOAD_OFFSET + 2u)
#define RB_ASSIGN_RETRY_COUNT_OFFSET (RB_ASSIGN_LINK_WINDOW_OFFSET + 1u)
#define RB_ASSIGN_LEASE_TIMEOUT_OFFSET (RB_ASSIGN_RETRY_COUNT_OFFSET + 1u)

#define RB_POLL_UPLINK_EPOCH_OFFSET RB_COMMON_HEADER_SIZE
#define RB_POLL_ACK_BASE_OFFSET (RB_POLL_UPLINK_EPOCH_OFFSET + 2u)
#define RB_POLL_ACK_BITMAP_OFFSET (RB_POLL_ACK_BASE_OFFSET + 4u)
#define RB_POLL_CREDIT_OFFSET (RB_POLL_ACK_BITMAP_OFFSET + 8u)
#define RB_POLL_SEQUENCE_OFFSET (RB_POLL_CREDIT_OFFSET + 2u)

#define RB_ACK_UPLINK_EPOCH_OFFSET RB_COMMON_HEADER_SIZE
#define RB_ACK_UPLINK_SEQUENCE_OFFSET (RB_ACK_UPLINK_EPOCH_OFFSET + 2u)
#define RB_ACK_DOWNLINK_EPOCH_OFFSET (RB_ACK_UPLINK_SEQUENCE_OFFSET + 4u)
#define RB_ACK_DOWNLINK_BASE_OFFSET (RB_ACK_DOWNLINK_EPOCH_OFFSET + 2u)
#define RB_ACK_DOWNLINK_BITMAP_OFFSET (RB_ACK_DOWNLINK_BASE_OFFSET + 4u)
#define RB_ACK_DROP_COUNT_OFFSET (RB_ACK_DOWNLINK_BITMAP_OFFSET + 8u)

#define RB_SKIP_DIRECTION_OFFSET RB_COMMON_HEADER_SIZE
#define RB_SKIP_EPOCH_OFFSET (RB_SKIP_DIRECTION_OFFSET + 1u)
#define RB_SKIP_NEXT_SEQUENCE_OFFSET (RB_SKIP_EPOCH_OFFSET + 2u)
#define RB_SKIP_DROP_COUNT_OFFSET (RB_SKIP_NEXT_SEQUENCE_OFFSET + 4u)

_Static_assert(RB_SYNC_RESPONSE_SLOT_US_OFFSET + 2u ==
		       RB_SYNC_DISCOVERY_WIRE_SIZE,
		       "sync discovery wire layout must remain 56 bytes");
_Static_assert(RB_DATA_SEQUENCE_OFFSET + 4u == RB_DATA_HEADER_SIZE,
		       "data wire layout must remain 18 byte header");
_Static_assert(RB_HELLO_CAPABILITIES_OFFSET + 4u == RB_HELLO_WIRE_SIZE,
		       "hello wire layout must remain 28 bytes");
_Static_assert(RB_ASSIGN_LEASE_TIMEOUT_OFFSET + 4u == RB_ASSIGN_WIRE_SIZE,
		       "assign wire layout must remain 34 bytes");
_Static_assert(RB_POLL_SEQUENCE_OFFSET + 2u == RB_POLL_WIRE_SIZE,
		       "poll wire layout must remain 30 bytes");
_Static_assert(RB_ACK_DROP_COUNT_OFFSET + 4u == RB_ACK_UPLINK_HEADER_SIZE,
		       "ack uplink wire header must remain 36 bytes");
_Static_assert(RB_SKIP_DROP_COUNT_OFFSET + 4u == RB_SKIP_TO_WIRE_SIZE,
		       "skip-to wire layout must remain 23 bytes");

static bool frame_type_valid(uint8_t type)
{
	switch (type) {
	case RB_FRAME_SYNC_DISCOVERY:
	case RB_FRAME_HELLO:
	case RB_FRAME_ASSIGN:
	case RB_FRAME_DOWNLINK_DATA:
	case RB_FRAME_REPAIR_DATA:
	case RB_FRAME_POLL:
	case RB_FRAME_ACK_UPLINK:
	case RB_FRAME_SKIP_TO:
		return true;
	default:
		return false;
	}
}

static bool data_type_valid(uint8_t type)
{
	return type == RB_FRAME_DOWNLINK_DATA || type == RB_FRAME_REPAIR_DATA;
}

static int common_arguments_valid(const struct rb_common_header *header)
{
	if (header == NULL || !frame_type_valid(header->type)) {
		return -EINVAL;
	}
	if (header->source_node > RB_MAX_SOURCE_NODE) {
		return -EINVAL;
	}

	return 0;
}

static int wire_common_valid(const uint8_t *wire, size_t wire_len)
{
	if (wire == NULL) {
		return -EINVAL;
	}
	if (wire_len > RB_ESB_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}
	if (wire_len < RB_COMMON_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	if (wire[0] != RB_PROTOCOL_MAGIC_0 || wire[1] != RB_PROTOCOL_MAGIC_1 ||
	    wire[2] != RB_PROTOCOL_VERSION) {
		return -EBADMSG;
	}
	if (!frame_type_valid(wire[RB_COMMON_TYPE_OFFSET])) {
		return -EINVAL;
	}
	if (wire[RB_COMMON_SOURCE_OFFSET] > RB_MAX_SOURCE_NODE) {
		return -EINVAL;
	}

	return 0;
}

int rb_common_encode(const struct rb_common_header *header,
			     uint8_t *wire, size_t wire_size)
{
	int ret;

	if (wire == NULL) {
		return -EINVAL;
	}
	if (wire_size < RB_COMMON_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	ret = common_arguments_valid(header);
	if (ret != 0) {
		return ret;
	}

	wire[0] = RB_PROTOCOL_MAGIC_0;
	wire[1] = RB_PROTOCOL_MAGIC_1;
	wire[2] = RB_PROTOCOL_VERSION;
	wire[RB_COMMON_TYPE_OFFSET] = header->type;
	sys_put_le32(header->master_session, &wire[RB_COMMON_SESSION_OFFSET]);
	sys_put_le16(header->lease_id, &wire[RB_COMMON_LEASE_OFFSET]);
	wire[RB_COMMON_SOURCE_OFFSET] = header->source_node;
	wire[RB_COMMON_FLAGS_OFFSET] = header->flags;

	return 0;
}

int rb_common_decode(const uint8_t *wire, size_t wire_len,
			     struct rb_common_header *header)
{
	struct rb_common_header decoded;
	int ret;

	if (header == NULL) {
		return -EINVAL;
	}
	ret = wire_common_valid(wire, wire_len);
	if (ret != 0) {
		return ret;
	}

	decoded.type = wire[RB_COMMON_TYPE_OFFSET];
	decoded.master_session = sys_get_le32(&wire[RB_COMMON_SESSION_OFFSET]);
	decoded.lease_id = sys_get_le16(&wire[RB_COMMON_LEASE_OFFSET]);
	decoded.source_node = wire[RB_COMMON_SOURCE_OFFSET];
	decoded.flags = wire[RB_COMMON_FLAGS_OFFSET];
	*header = decoded;

	return 0;
}

int rb_sync_discovery_encode(const struct rb_sync_discovery *frame,
				     uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || wire == NULL || wire_len == NULL) {
		return -EINVAL;
	}
	if (wire_size < RB_SYNC_DISCOVERY_WIRE_SIZE) {
		return -EMSGSIZE;
	}
	ret = common_arguments_valid(&frame->common);
	if (ret != 0) {
		return ret;
	}
	if (frame->common.type != RB_FRAME_SYNC_DISCOVERY ||
	    frame->common.source_node != 0 || frame->common.lease_id != 0) {
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
	sys_put_le32(frame->discovery_nonce, &wire[RB_SYNC_NONCE_OFFSET]);
	wire[RB_SYNC_FREE_SLOTS_OFFSET] = frame->free_slots;
	wire[RB_SYNC_RESPONSE_COUNT_OFFSET] = frame->response_slot_count;
	sys_put_le16(frame->response_slot_us, &wire[RB_SYNC_RESPONSE_SLOT_US_OFFSET]);
	if (wire_len != NULL) {
		*wire_len = RB_SYNC_DISCOVERY_WIRE_SIZE;
	}

	return 0;
}

int rb_sync_discovery_decode(const uint8_t *wire, size_t wire_len,
				     struct rb_sync_discovery *frame)
{
	struct rb_common_header common;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	if (wire_len != RB_SYNC_DISCOVERY_WIRE_SIZE) {
		return -EMSGSIZE;
	}
	ret = rb_common_decode(wire, wire_len, &common);
	if (ret != 0) {
		return ret;
	}
	if (common.type != RB_FRAME_SYNC_DISCOVERY || common.source_node != 0 ||
	    common.lease_id != 0) {
		return -EINVAL;
	}

	frame->common = common;
	frame->group_id = sys_get_le32(&wire[RB_SYNC_GROUP_OFFSET]);
	frame->master_id = sys_get_le64(&wire[RB_SYNC_MASTER_ID_OFFSET]);
	frame->sync_sequence = sys_get_le32(&wire[RB_SYNC_SEQUENCE_OFFSET]);
	frame->previous_master_address_tick =
		sys_get_le64(&wire[RB_SYNC_PREVIOUS_TICK_OFFSET]);
	frame->next_pps_master_tick = sys_get_le64(&wire[RB_SYNC_NEXT_PPS_OFFSET]);
	frame->sync_interval_us = sys_get_le32(&wire[RB_SYNC_INTERVAL_OFFSET]);
	frame->discovery_nonce = sys_get_le32(&wire[RB_SYNC_NONCE_OFFSET]);
	frame->free_slots = wire[RB_SYNC_FREE_SLOTS_OFFSET];
	frame->response_slot_count = wire[RB_SYNC_RESPONSE_COUNT_OFFSET];
	frame->response_slot_us = sys_get_le16(&wire[RB_SYNC_RESPONSE_SLOT_US_OFFSET]);

	return 0;
}

int rb_data_encode(uint8_t type, uint32_t master_session, uint16_t lease_id,
			   uint16_t stream_epoch, uint32_t sequence,
			   const uint8_t *payload, size_t payload_len,
			   uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	struct rb_common_header common;
	size_t total_len;
	int ret;

	if (wire == NULL || wire_len == NULL || !data_type_valid(type)) {
		return -EINVAL;
	}
	if (payload == NULL && payload_len != 0) {
		return -EINVAL;
	}
	if (stream_epoch == 0) {
		return -EINVAL;
	}
	if (payload_len > RB_ESB_MAX_PAYLOAD - RB_DATA_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	total_len = RB_DATA_HEADER_SIZE + payload_len;
	if (wire_size < total_len) {
		return -EMSGSIZE;
	}

	common = RB_COMMON_INIT(type, master_session, lease_id, 0);
	ret = rb_common_encode(&common, wire, wire_size);
	if (ret != 0) {
		return ret;
	}
	sys_put_le16(stream_epoch, &wire[RB_DATA_EPOCH_OFFSET]);
	sys_put_le32(sequence, &wire[RB_DATA_SEQUENCE_OFFSET]);
	if (payload_len != 0) {
		memcpy(&wire[RB_DATA_HEADER_SIZE], payload, payload_len);
	}
	*wire_len = total_len;

	return 0;
}

int rb_data_decode(const uint8_t *wire, size_t wire_len,
			  struct rb_data_frame *frame)
{
	struct rb_common_header common;
	uint16_t stream_epoch;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	if (wire_len < RB_DATA_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	ret = rb_common_decode(wire, wire_len, &common);
	if (ret != 0) {
		return ret;
	}
	if (!data_type_valid(common.type)) {
		return -EINVAL;
	}
	stream_epoch = sys_get_le16(&wire[RB_DATA_EPOCH_OFFSET]);
	if (stream_epoch == 0) {
		return -EINVAL;
	}

	frame->common = common;
	frame->stream_epoch = stream_epoch;
	frame->sequence = sys_get_le32(&wire[RB_DATA_SEQUENCE_OFFSET]);
	frame->payload = &wire[RB_DATA_HEADER_SIZE];
	frame->payload_len = wire_len - RB_DATA_HEADER_SIZE;

	return 0;
}

static int fixed_control_encode(const struct rb_common_header *common,
				uint8_t expected_type, uint8_t *wire,
				size_t wire_size, size_t required_size,
				size_t *wire_len)
{
	int ret;

	if (wire == NULL || wire_len == NULL) {
		return -EINVAL;
	}
	if (wire_size < required_size) {
		return -EMSGSIZE;
	}
	ret = common_arguments_valid(common);
	if (ret != 0) {
		return ret;
	}
	if (common->type != expected_type) {
		return -EINVAL;
	}

	return rb_common_encode(common, wire, wire_size);
}

static int fixed_control_decode(const uint8_t *wire, size_t wire_len,
				uint8_t expected_type, size_t required_size,
				struct rb_common_header *common)
{
	int ret;

	if (wire_len != required_size) {
		return -EMSGSIZE;
	}
	ret = rb_common_decode(wire, wire_len, common);
	if (ret != 0) {
		return ret;
	}
	if (common->type != expected_type) {
		return -EINVAL;
	}

	return 0;
}

static bool hello_common_valid(const struct rb_common_header *common)
{
	return common->type == RB_FRAME_HELLO && common->lease_id == 0 &&
		common->source_node == 0;
}

int rb_hello_encode(const struct rb_hello *frame,
			uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || !hello_common_valid(&frame->common)) {
		return -EINVAL;
	}
	ret = fixed_control_encode(&frame->common, RB_FRAME_HELLO, wire,
				   wire_size, RB_HELLO_WIRE_SIZE, wire_len);
	if (ret != 0) {
		return ret;
	}

	sys_put_le64(frame->device_id, &wire[RB_HELLO_DEVICE_ID_OFFSET]);
	sys_put_le32(frame->discovery_nonce, &wire[RB_HELLO_NONCE_OFFSET]);
	sys_put_le32(frame->capabilities, &wire[RB_HELLO_CAPABILITIES_OFFSET]);
	*wire_len = RB_HELLO_WIRE_SIZE;

	return 0;
}

int rb_hello_decode(const uint8_t *wire, size_t wire_len,
			struct rb_hello *frame)
{
	struct rb_hello decoded;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	ret = fixed_control_decode(wire, wire_len, RB_FRAME_HELLO,
				   RB_HELLO_WIRE_SIZE, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	if (!hello_common_valid(&decoded.common)) {
		return -EINVAL;
	}

	decoded.device_id = sys_get_le64(&wire[RB_HELLO_DEVICE_ID_OFFSET]);
	decoded.discovery_nonce = sys_get_le32(&wire[RB_HELLO_NONCE_OFFSET]);
	decoded.capabilities = sys_get_le32(&wire[RB_HELLO_CAPABILITIES_OFFSET]);
	*frame = decoded;

	return 0;
}

static bool assign_fields_valid(const struct rb_assign *frame)
{
	return frame->common.type == RB_FRAME_ASSIGN &&
		frame->common.source_node == 0 && frame->common.lease_id != 0 &&
		frame->downlink_epoch != 0 && frame->uplink_epoch != 0 &&
		frame->node_id >= 1 && frame->node_id <= RB_MAX_SOURCE_NODE &&
		frame->pipe >= 1 && frame->pipe <= RB_MAX_SOURCE_NODE &&
		frame->node_id == frame->pipe;
}

int rb_assign_encode(const struct rb_assign *frame,
			 uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || !assign_fields_valid(frame)) {
		return -EINVAL;
	}
	ret = fixed_control_encode(&frame->common, RB_FRAME_ASSIGN, wire,
				   wire_size, RB_ASSIGN_WIRE_SIZE, wire_len);
	if (ret != 0) {
		return ret;
	}

	sys_put_le64(frame->target_device_id, &wire[RB_ASSIGN_TARGET_ID_OFFSET]);
	wire[RB_ASSIGN_NODE_OFFSET] = frame->node_id;
	wire[RB_ASSIGN_PIPE_OFFSET] = frame->pipe;
	sys_put_le16(frame->downlink_epoch, &wire[RB_ASSIGN_DOWNLINK_EPOCH_OFFSET]);
	sys_put_le16(frame->uplink_epoch, &wire[RB_ASSIGN_UPLINK_EPOCH_OFFSET]);
	sys_put_le16(frame->max_payload, &wire[RB_ASSIGN_MAX_PAYLOAD_OFFSET]);
	wire[RB_ASSIGN_LINK_WINDOW_OFFSET] = frame->link_window;
	wire[RB_ASSIGN_RETRY_COUNT_OFFSET] = frame->retry_count;
	sys_put_le32(frame->lease_timeout_us, &wire[RB_ASSIGN_LEASE_TIMEOUT_OFFSET]);
	*wire_len = RB_ASSIGN_WIRE_SIZE;

	return 0;
}

int rb_assign_decode(const uint8_t *wire, size_t wire_len,
			 struct rb_assign *frame)
{
	struct rb_assign decoded;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	ret = fixed_control_decode(wire, wire_len, RB_FRAME_ASSIGN,
				   RB_ASSIGN_WIRE_SIZE, &decoded.common);
	if (ret != 0) {
		return ret;
	}

	decoded.target_device_id = sys_get_le64(&wire[RB_ASSIGN_TARGET_ID_OFFSET]);
	decoded.node_id = wire[RB_ASSIGN_NODE_OFFSET];
	decoded.pipe = wire[RB_ASSIGN_PIPE_OFFSET];
	decoded.downlink_epoch =
		sys_get_le16(&wire[RB_ASSIGN_DOWNLINK_EPOCH_OFFSET]);
	decoded.uplink_epoch = sys_get_le16(&wire[RB_ASSIGN_UPLINK_EPOCH_OFFSET]);
	decoded.max_payload = sys_get_le16(&wire[RB_ASSIGN_MAX_PAYLOAD_OFFSET]);
	decoded.link_window = wire[RB_ASSIGN_LINK_WINDOW_OFFSET];
	decoded.retry_count = wire[RB_ASSIGN_RETRY_COUNT_OFFSET];
	decoded.lease_timeout_us =
		sys_get_le32(&wire[RB_ASSIGN_LEASE_TIMEOUT_OFFSET]);
	if (!assign_fields_valid(&decoded)) {
		return -EINVAL;
	}
	*frame = decoded;

	return 0;
}

int rb_poll_encode(const struct rb_poll *frame,
		       uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || frame->uplink_epoch == 0) {
		return -EINVAL;
	}
	ret = fixed_control_encode(&frame->common, RB_FRAME_POLL, wire,
				   wire_size, RB_POLL_WIRE_SIZE, wire_len);
	if (ret != 0) {
		return ret;
	}

	sys_put_le16(frame->uplink_epoch, &wire[RB_POLL_UPLINK_EPOCH_OFFSET]);
	sys_put_le32(frame->uplink_ack_base, &wire[RB_POLL_ACK_BASE_OFFSET]);
	sys_put_le64(frame->uplink_ack_bitmap, &wire[RB_POLL_ACK_BITMAP_OFFSET]);
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

	if (frame == NULL) {
		return -EINVAL;
	}
	ret = fixed_control_decode(wire, wire_len, RB_FRAME_POLL,
				   RB_POLL_WIRE_SIZE, &decoded.common);
	if (ret != 0) {
		return ret;
	}

	decoded.uplink_epoch = sys_get_le16(&wire[RB_POLL_UPLINK_EPOCH_OFFSET]);
	if (decoded.uplink_epoch == 0) {
		return -EINVAL;
	}
	decoded.uplink_ack_base = sys_get_le32(&wire[RB_POLL_ACK_BASE_OFFSET]);
	decoded.uplink_ack_bitmap = sys_get_le64(&wire[RB_POLL_ACK_BITMAP_OFFSET]);
	decoded.next_credit_bytes = sys_get_le16(&wire[RB_POLL_CREDIT_OFFSET]);
	decoded.poll_sequence = sys_get_le16(&wire[RB_POLL_SEQUENCE_OFFSET]);
	*frame = decoded;

	return 0;
}

int rb_ack_uplink_encode(const struct rb_ack_uplink *frame,
			     uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	size_t total_len;
	int ret;

	if (frame == NULL || frame->uplink_epoch == 0 ||
	    frame->downlink_epoch == 0) {
		return -EINVAL;
	}
	if (frame->payload == NULL && frame->payload_len != 0) {
		return -EINVAL;
	}
	if (frame->payload_len > RB_ESB_MAX_PAYLOAD - RB_ACK_UPLINK_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	total_len = RB_ACK_UPLINK_HEADER_SIZE + frame->payload_len;
	ret = fixed_control_encode(&frame->common, RB_FRAME_ACK_UPLINK, wire,
				   wire_size, total_len, wire_len);
	if (ret != 0) {
		return ret;
	}

	sys_put_le16(frame->uplink_epoch, &wire[RB_ACK_UPLINK_EPOCH_OFFSET]);
	sys_put_le32(frame->uplink_sequence, &wire[RB_ACK_UPLINK_SEQUENCE_OFFSET]);
	sys_put_le16(frame->downlink_epoch, &wire[RB_ACK_DOWNLINK_EPOCH_OFFSET]);
	sys_put_le32(frame->downlink_ack_base, &wire[RB_ACK_DOWNLINK_BASE_OFFSET]);
	sys_put_le64(frame->downlink_ack_bitmap,
		     &wire[RB_ACK_DOWNLINK_BITMAP_OFFSET]);
	sys_put_le32(frame->drop_count, &wire[RB_ACK_DROP_COUNT_OFFSET]);
	if (frame->payload_len != 0) {
		memcpy(&wire[RB_ACK_UPLINK_HEADER_SIZE], frame->payload,
		       frame->payload_len);
	}
	*wire_len = total_len;

	return 0;
}

int rb_ack_uplink_decode(const uint8_t *wire, size_t wire_len,
			     struct rb_ack_uplink *frame)
{
	struct rb_ack_uplink decoded;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	if (wire_len < RB_ACK_UPLINK_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	ret = rb_common_decode(wire, wire_len, &decoded.common);
	if (ret != 0) {
		return ret;
	}
	if (decoded.common.type != RB_FRAME_ACK_UPLINK) {
		return -EINVAL;
	}

	decoded.uplink_epoch = sys_get_le16(&wire[RB_ACK_UPLINK_EPOCH_OFFSET]);
	decoded.downlink_epoch = sys_get_le16(&wire[RB_ACK_DOWNLINK_EPOCH_OFFSET]);
	if (decoded.uplink_epoch == 0 || decoded.downlink_epoch == 0) {
		return -EINVAL;
	}
	decoded.uplink_sequence =
		sys_get_le32(&wire[RB_ACK_UPLINK_SEQUENCE_OFFSET]);
	decoded.downlink_ack_base = sys_get_le32(&wire[RB_ACK_DOWNLINK_BASE_OFFSET]);
	decoded.downlink_ack_bitmap =
		sys_get_le64(&wire[RB_ACK_DOWNLINK_BITMAP_OFFSET]);
	decoded.drop_count = sys_get_le32(&wire[RB_ACK_DROP_COUNT_OFFSET]);
	decoded.payload = &wire[RB_ACK_UPLINK_HEADER_SIZE];
	decoded.payload_len = wire_len - RB_ACK_UPLINK_HEADER_SIZE;
	*frame = decoded;

	return 0;
}

int rb_skip_to_encode(const struct rb_skip_to *frame,
			  uint8_t *wire, size_t wire_size, size_t *wire_len)
{
	int ret;

	if (frame == NULL || frame->stream_epoch == 0) {
		return -EINVAL;
	}
	ret = fixed_control_encode(&frame->common, RB_FRAME_SKIP_TO, wire,
				   wire_size, RB_SKIP_TO_WIRE_SIZE, wire_len);
	if (ret != 0) {
		return ret;
	}

	wire[RB_SKIP_DIRECTION_OFFSET] = frame->direction;
	sys_put_le16(frame->stream_epoch, &wire[RB_SKIP_EPOCH_OFFSET]);
	sys_put_le32(frame->next_sequence, &wire[RB_SKIP_NEXT_SEQUENCE_OFFSET]);
	sys_put_le32(frame->drop_count, &wire[RB_SKIP_DROP_COUNT_OFFSET]);
	*wire_len = RB_SKIP_TO_WIRE_SIZE;

	return 0;
}

int rb_skip_to_decode(const uint8_t *wire, size_t wire_len,
			  struct rb_skip_to *frame)
{
	struct rb_skip_to decoded;
	int ret;

	if (frame == NULL) {
		return -EINVAL;
	}
	ret = fixed_control_decode(wire, wire_len, RB_FRAME_SKIP_TO,
				   RB_SKIP_TO_WIRE_SIZE, &decoded.common);
	if (ret != 0) {
		return ret;
	}

	decoded.direction = wire[RB_SKIP_DIRECTION_OFFSET];
	decoded.stream_epoch = sys_get_le16(&wire[RB_SKIP_EPOCH_OFFSET]);
	if (decoded.stream_epoch == 0) {
		return -EINVAL;
	}
	decoded.next_sequence = sys_get_le32(&wire[RB_SKIP_NEXT_SEQUENCE_OFFSET]);
	decoded.drop_count = sys_get_le32(&wire[RB_SKIP_DROP_COUNT_OFFSET]);
	*frame = decoded;

	return 0;
}
