#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "link_protocol.h"

ZTEST(link_protocol, test_common_header_uses_little_endian_wire_layout)
{
	struct rb_common_header in = RB_COMMON_INIT(RB_FRAME_DOWNLINK_DATA,
							 0x11223344, 0x5566, 2);
	uint8_t wire[RB_COMMON_HEADER_SIZE];
	struct rb_common_header out;

	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	zassert_equal(wire[0], 'R');
	zassert_equal(wire[1], 'B');
	zassert_equal(wire[2], RB_PROTOCOL_VERSION);
	zassert_equal(wire[3], RB_FRAME_DOWNLINK_DATA);
	zassert_equal(wire[4], 0x44);
	zassert_equal(wire[5], 0x33);
	zassert_equal(wire[6], 0x22);
	zassert_equal(wire[7], 0x11);
	zassert_equal(wire[8], 0x66);
	zassert_equal(wire[9], 0x55);
	zassert_equal(wire[10], 2);
	zassert_equal(wire[11], 0);
	zassert_ok(rb_common_decode(wire, sizeof(wire), &out));
	zassert_equal(out.master_session, in.master_session);
	zassert_equal(out.lease_id, in.lease_id);
	zassert_equal(out.source_node, in.source_node);
}

ZTEST(link_protocol, test_sync_discovery_round_trip)
{
	struct rb_sync_discovery in = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 0x11223344, 0, 0),
		.group_id = 41,
		.master_id = 0x0123456789abcdefULL,
		.sync_sequence = 8,
		.previous_master_address_tick = 123456789ULL,
		.next_pps_master_tick = 124000000ULL,
		.sync_interval_us = 100000,
		.discovery_nonce = 0xaabbccdd,
		.free_slots = 2,
		.response_slot_count = 8,
		.response_slot_us = 500,
		.next_pps_utc_seconds = 1700000000,
		.time_quality = RB_TIME_LOCKED,
	};
	struct rb_sync_discovery out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_sync_discovery_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_SYNC_DISCOVERY_WIRE_SIZE);
	zassert_ok(rb_sync_discovery_decode(wire, len, &out));
	zassert_equal(out.group_id, 41);
	zassert_equal(out.master_id, in.master_id);
	zassert_equal(out.sync_sequence, in.sync_sequence);
	zassert_equal(out.previous_master_address_tick, 123456789ULL);
	zassert_equal(out.next_pps_master_tick, 124000000ULL);
	zassert_equal(out.sync_interval_us, 100000);
	zassert_equal(out.discovery_nonce, in.discovery_nonce);
	zassert_equal(out.free_slots, 2);
	zassert_equal(out.response_slot_count, 8);
	zassert_equal(out.response_slot_us, 500);
	zassert_equal(out.next_pps_utc_seconds, 1700000000);
	zassert_equal(out.time_quality, RB_TIME_LOCKED);
	/* The signed UTC value is carried as a little-endian two's-complement
	 * 64-bit integer immediately after the legacy 56-byte payload. */
	zassert_equal(wire[56], 0x00);
	zassert_equal(wire[57], 0xf1);
	zassert_equal(wire[58], 0x53);
	zassert_equal(wire[59], 0x65);
	zassert_equal(wire[60], 0x00);
	zassert_equal(wire[61], 0x00);
	zassert_equal(wire[62], 0x00);
	zassert_equal(wire[63], 0x00);
	zassert_equal(wire[64], RB_TIME_LOCKED);
}

ZTEST(link_protocol, test_sync_discovery_rejects_invalid_quality_and_v1)
{
	struct rb_sync_discovery frame = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 1, 0, 0),
		.group_id = 1,
		.master_id = 1,
		.sync_sequence = 1,
		.next_pps_master_tick = 2,
		.next_pps_utc_seconds = -1,
		.time_quality = RB_TIME_LOCKED,
	};
	struct rb_sync_discovery out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_sync_discovery_encode(&frame, wire, sizeof(wire), &len));
	zassert_equal(rb_sync_discovery_decode(wire, len, &out), 0);
	zassert_equal(out.next_pps_utc_seconds, -1);
	zassert_equal(wire[56], 0xff);
	zassert_equal(wire[63], 0xff);
	wire[64] = 0xff;
	zassert_equal(rb_sync_discovery_decode(wire, len, &out), -EINVAL);
	wire[2] = 1;
	zassert_equal(rb_sync_discovery_decode(wire, len, &out), -EBADMSG);
}

ZTEST(link_protocol, test_downlink_keeps_opaque_bytes)
{
	const uint8_t bytes[] = {0x00, 0xfd, 0x7e, 0xff, 0x00};
	struct rb_data_frame out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 7, 0, 9, 12,
				 bytes, sizeof(bytes), wire, sizeof(wire), &len));
	zassert_ok(rb_data_decode(wire, len, &out));
	zassert_equal(out.common.type, RB_FRAME_DOWNLINK_DATA);
	zassert_equal(out.common.master_session, 7);
	zassert_equal(out.common.lease_id, 0);
	zassert_equal(out.stream_epoch, 9);
	zassert_equal(out.sequence, 12);
	zassert_equal(out.payload_len, sizeof(bytes));
	zassert_mem_equal(out.payload, bytes, sizeof(bytes));
}

ZTEST(link_protocol, test_common_decode_rejects_bad_magic_and_version)
{
	struct rb_common_header in = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 1, 0, 0);
	struct rb_common_header out;
	uint8_t wire[RB_COMMON_HEADER_SIZE];

	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	wire[0] = 'X';
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EBADMSG);
	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	wire[2]++;
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EBADMSG);
}

ZTEST(link_protocol, test_common_decode_rejects_invalid_type_source_and_length)
{
	struct rb_common_header in = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 1, 0, 0);
	struct rb_common_header out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD + 1];

	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	wire[3] = 0xff;
	zassert_equal(rb_common_decode(wire, RB_COMMON_HEADER_SIZE, &out), -EINVAL);
	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	wire[10] = RB_MAX_SOURCE_NODE + 1;
	zassert_equal(rb_common_decode(wire, RB_COMMON_HEADER_SIZE, &out), -EINVAL);
	zassert_equal(rb_common_decode(wire, RB_COMMON_HEADER_SIZE - 1, &out),
			      -EMSGSIZE);
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EMSGSIZE);
}

ZTEST(link_protocol, test_sync_decode_rejects_wrong_type_and_truncation)
{
	struct rb_sync_discovery in = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC_DISCOVERY, 1, 0, 0),
		.group_id = 1,
		.master_id = 1,
		.sync_sequence = 1,
		.previous_master_address_tick = 1,
		.next_pps_master_tick = 2,
		.sync_interval_us = 3,
		.discovery_nonce = 4,
		.free_slots = 1,
		.response_slot_count = 1,
		.response_slot_us = 5,
	};
	struct rb_sync_discovery out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_sync_discovery_encode(&in, wire, sizeof(wire), &len));
	wire[3] = RB_FRAME_DOWNLINK_DATA;
	zassert_equal(rb_sync_discovery_decode(wire, len, &out), -EINVAL);
	zassert_ok(rb_sync_discovery_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(rb_sync_discovery_decode(wire, len - 1, &out), -EMSGSIZE);
}

ZTEST(link_protocol, test_data_encode_rejects_oversize_and_null_payload)
{
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	uint8_t payload[RB_ESB_MAX_PAYLOAD] = {0};
	size_t len;

	zassert_equal(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 1, 0, 1, 1,
				     payload, sizeof(payload), wire, sizeof(wire), &len),
			     -EMSGSIZE);
	zassert_equal(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 1, 0, 1, 1,
				     NULL, 1, wire, sizeof(wire), &len), -EINVAL);
	zassert_equal(rb_data_encode(RB_FRAME_SYNC_DISCOVERY, 1, 0, 1, 1,
				     NULL, 0, wire, sizeof(wire), &len), -EINVAL);
}

ZTEST(link_protocol, test_data_decode_rejects_wrong_length_and_type)
{
	uint8_t wire[RB_ESB_MAX_PAYLOAD + 1] = {0};
	struct rb_data_frame out;
	size_t len;

	zassert_ok(rb_data_encode(RB_FRAME_DOWNLINK_DATA, 1, 0, 1, 1,
				 NULL, 0, wire, sizeof(wire), &len));
	zassert_equal(rb_data_decode(wire, RB_DATA_HEADER_SIZE - 1, &out),
			      -EMSGSIZE);
	zassert_equal(rb_data_decode(wire, sizeof(wire), &out), -EMSGSIZE);
	wire[3] = RB_FRAME_SYNC_DISCOVERY;
	zassert_equal(rb_data_decode(wire, len, &out), -EINVAL);
}

ZTEST(link_protocol, test_hello_round_trip)
{
	struct rb_hello in = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 0x11223344, 0, 0),
		.device_id = UINT64_C(0x0123456789abcdef),
		.discovery_nonce = 0xa1b2c3d4,
		.capabilities = 0x55667788,
	};
	struct rb_hello out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_hello_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_HELLO_WIRE_SIZE);
	zassert_equal(wire[12], 0xef);
	zassert_equal(wire[19], 0x01);
	zassert_equal(wire[20], 0xd4);
	zassert_equal(wire[27], 0x55);
	zassert_ok(rb_hello_decode(wire, len, &out));
	zassert_equal(out.common.master_session, in.common.master_session);
	zassert_equal(out.common.lease_id, 0);
	zassert_equal(out.common.source_node, 0);
	zassert_equal(out.device_id, in.device_id);
	zassert_equal(out.discovery_nonce, in.discovery_nonce);
	zassert_equal(out.capabilities, in.capabilities);
}

ZTEST(link_protocol, test_assign_round_trip)
{
	struct rb_assign in = {
		.common = RB_COMMON_INIT(RB_FRAME_ASSIGN, 0x10203040, 0x5060, 0),
		.target_device_id = UINT64_C(0xfedcba9876543210),
		.node_id = 2,
		.pipe = 2,
		.downlink_epoch = 0x1122,
		.uplink_epoch = 0x3344,
		.max_payload = 0x5566,
		.link_window = 0x77,
		.retry_count = 0x88,
		.lease_timeout_us = 0x99aabbcc,
	};
	struct rb_assign out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_assign_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_ASSIGN_WIRE_SIZE);
	zassert_equal(wire[12], 0x10);
	zassert_equal(wire[19], 0xfe);
	zassert_equal(wire[20], 2);
	zassert_equal(wire[21], 2);
	zassert_equal(wire[22], 0x22);
	zassert_equal(wire[24], 0x44);
	zassert_equal(wire[26], 0x66);
	zassert_equal(wire[30], 0xcc);
	zassert_equal(wire[33], 0x99);
	zassert_ok(rb_assign_decode(wire, len, &out));
	zassert_equal(out.common.master_session, in.common.master_session);
	zassert_equal(out.common.lease_id, in.common.lease_id);
	zassert_equal(out.target_device_id, in.target_device_id);
	zassert_equal(out.node_id, in.node_id);
	zassert_equal(out.pipe, in.pipe);
	zassert_equal(out.downlink_epoch, in.downlink_epoch);
	zassert_equal(out.uplink_epoch, in.uplink_epoch);
	zassert_equal(out.max_payload, in.max_payload);
	zassert_equal(out.link_window, in.link_window);
	zassert_equal(out.retry_count, in.retry_count);
	zassert_equal(out.lease_timeout_us, in.lease_timeout_us);
}

ZTEST(link_protocol, test_poll_round_trip)
{
	struct rb_poll in = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 0x12345678, 0xabcd, 0),
		.uplink_epoch = 0x1020,
		.uplink_ack_base = 0x30405060,
		.uplink_ack_bitmap = UINT64_C(0x8877665544332211),
		.next_credit_bytes = 0x90a0,
		.poll_sequence = 0xb0c0,
	};
	struct rb_poll out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_poll_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_POLL_WIRE_SIZE);
	zassert_equal(wire[12], 0x20);
	zassert_equal(wire[14], 0x60);
	zassert_equal(wire[18], 0x11);
	zassert_equal(wire[25], 0x88);
	zassert_equal(wire[28], 0xc0);
	zassert_ok(rb_poll_decode(wire, len, &out));
	zassert_equal(out.common.master_session, in.common.master_session);
	zassert_equal(out.common.lease_id, in.common.lease_id);
	zassert_equal(out.uplink_epoch, in.uplink_epoch);
	zassert_equal(out.uplink_ack_base, in.uplink_ack_base);
	zassert_equal(out.uplink_ack_bitmap, in.uplink_ack_bitmap);
	zassert_equal(out.next_credit_bytes, in.next_credit_bytes);
	zassert_equal(out.poll_sequence, in.poll_sequence);
}

ZTEST(link_protocol, test_ack_uplink_round_trip)
{
	const uint8_t bytes[] = {1, 2, 3, 4};
	struct rb_ack_uplink in = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 0x1234, 0x55aa, 2),
		.uplink_epoch = 4,
		.uplink_sequence = 91,
		.downlink_epoch = 8,
		.downlink_ack_base = 77,
		.downlink_ack_bitmap = UINT64_C(0x8000000000000005),
		.drop_count = 3,
		.payload = bytes,
		.payload_len = sizeof(bytes),
	};
	struct rb_ack_uplink out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_ack_uplink_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_ACK_UPLINK_HEADER_SIZE + sizeof(bytes));
	zassert_equal(wire[24], 0x05);
	zassert_equal(wire[31], 0x80);
	zassert_equal(wire[32], 0x03);
	zassert_equal(wire[35], 0x00);
	zassert_ok(rb_ack_uplink_decode(wire, len, &out));
	zassert_equal(out.common.source_node, 2);
	zassert_equal(out.uplink_epoch, in.uplink_epoch);
	zassert_equal(out.uplink_sequence, in.uplink_sequence);
	zassert_equal(out.downlink_epoch, in.downlink_epoch);
	zassert_equal(out.downlink_ack_base, in.downlink_ack_base);
	zassert_equal(out.downlink_ack_bitmap, in.downlink_ack_bitmap);
	zassert_equal(out.drop_count, in.drop_count);
	zassert_equal(out.payload_len, sizeof(bytes));
	zassert_mem_equal(out.payload, bytes, sizeof(bytes));
}

ZTEST(link_protocol, test_skip_to_round_trip)
{
	struct rb_skip_to in = {
		.common = RB_COMMON_INIT(RB_FRAME_SKIP_TO, 0x01020304, 0x0506, 3),
		.direction = 0x07,
		.stream_epoch = 0x0809,
		.next_sequence = 0x0a0b0c0d,
		.drop_count = 0x0e0f1011,
	};
	struct rb_skip_to out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len = 0;

	zassert_ok(rb_skip_to_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_SKIP_TO_WIRE_SIZE);
	zassert_equal(wire[12], 0x07);
	zassert_equal(wire[13], 0x09);
	zassert_equal(wire[15], 0x0d);
	zassert_equal(wire[19], 0x11);
	zassert_equal(wire[22], 0x0e);
	zassert_ok(rb_skip_to_decode(wire, len, &out));
	zassert_equal(out.common.master_session, in.common.master_session);
	zassert_equal(out.common.lease_id, in.common.lease_id);
	zassert_equal(out.common.source_node, in.common.source_node);
	zassert_equal(out.direction, in.direction);
	zassert_equal(out.stream_epoch, in.stream_epoch);
	zassert_equal(out.next_sequence, in.next_sequence);
	zassert_equal(out.drop_count, in.drop_count);
}

ZTEST(link_protocol, test_hello_rejects_non_candidate_header)
{
	struct rb_hello frame = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 1, 1, 0),
		.device_id = 2,
		.discovery_nonce = 3,
		.capabilities = 4,
	};
	struct rb_hello out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_equal(rb_hello_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.common.lease_id = 0;
	frame.common.source_node = 1;
	zassert_equal(rb_hello_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.common.source_node = 0;
	zassert_ok(rb_hello_encode(&frame, wire, sizeof(wire), &len));
	wire[8] = 1;
	zassert_equal(rb_hello_decode(wire, len, &out), -EINVAL);
	zassert_ok(rb_hello_encode(&frame, wire, sizeof(wire), &len));
	wire[10] = 1;
	zassert_equal(rb_hello_decode(wire, len, &out), -EINVAL);
}

ZTEST(link_protocol, test_assign_rejects_invalid_lease_epoch_node_and_pipe)
{
	struct rb_assign frame = {
		.common = RB_COMMON_INIT(RB_FRAME_ASSIGN, 1, 2, 0),
		.target_device_id = 3,
		.node_id = 1,
		.pipe = 1,
		.downlink_epoch = 4,
		.uplink_epoch = 5,
		.max_payload = 6,
		.link_window = 7,
		.retry_count = 8,
		.lease_timeout_us = 9,
	};
	struct rb_assign out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	frame.common.lease_id = 0;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.common.lease_id = 2;
	frame.downlink_epoch = 0;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.downlink_epoch = 4;
	frame.uplink_epoch = 0;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.uplink_epoch = 5;
	frame.node_id = 0;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.node_id = 4;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.node_id = 1;
	frame.pipe = 0;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.pipe = 4;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.pipe = 2;
	zassert_equal(rb_assign_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.pipe = 1;
	zassert_ok(rb_assign_encode(&frame, wire, sizeof(wire), &len));
	wire[8] = 0;
	wire[9] = 0;
	zassert_equal(rb_assign_decode(wire, len, &out), -EINVAL);
	zassert_ok(rb_assign_encode(&frame, wire, sizeof(wire), &len));
	wire[20] = 2;
	zassert_equal(rb_assign_decode(wire, len, &out), -EINVAL);
}

ZTEST(link_protocol, test_control_codecs_reject_wrong_type_and_length)
{
	struct rb_hello hello = {
		.common = RB_COMMON_INIT(RB_FRAME_HELLO, 1, 0, 0),
	};
	struct rb_hello hello_out;
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 1, 2, 0),
		.uplink_epoch = 3,
	};
	struct rb_poll poll_out;
	struct rb_skip_to skip = {
		.common = RB_COMMON_INIT(RB_FRAME_SKIP_TO, 1, 2, 1),
		.direction = 1,
		.stream_epoch = 3,
	};
	struct rb_skip_to skip_out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD + 1];
	size_t len;

	zassert_ok(rb_hello_encode(&hello, wire, sizeof(wire), &len));
	zassert_equal(rb_hello_decode(wire, len - 1, &hello_out), -EMSGSIZE);
	wire[3] = RB_FRAME_POLL;
	zassert_equal(rb_hello_decode(wire, len, &hello_out), -EINVAL);
	zassert_ok(rb_poll_encode(&poll, wire, sizeof(wire), &len));
	zassert_equal(rb_poll_decode(wire, len - 1, &poll_out), -EMSGSIZE);
	wire[3] = RB_FRAME_HELLO;
	zassert_equal(rb_poll_decode(wire, len, &poll_out), -EINVAL);
	zassert_ok(rb_skip_to_encode(&skip, wire, sizeof(wire), &len));
	zassert_equal(rb_skip_to_decode(wire, len - 1, &skip_out), -EMSGSIZE);
	wire[3] = RB_FRAME_ACK_UPLINK;
	zassert_equal(rb_skip_to_decode(wire, len, &skip_out), -EINVAL);
}

ZTEST(link_protocol, test_ack_payload_uses_esb_frame_length)
{
	const uint8_t bytes[] = {0xaa, 0xbb, 0xcc};
	struct rb_ack_uplink frame = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 1, 2, 1),
		.uplink_epoch = 3,
		.uplink_sequence = 4,
		.downlink_epoch = 5,
		.downlink_ack_base = 6,
		.downlink_ack_bitmap = 7,
		.drop_count = 8,
		.payload = bytes,
		.payload_len = sizeof(bytes),
	};
	struct rb_ack_uplink out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD + 1];
	uint8_t oversized[RB_ESB_MAX_PAYLOAD] = {0};
	size_t len;

	zassert_ok(rb_ack_uplink_encode(&frame, wire, sizeof(wire), &len));
	zassert_ok(rb_ack_uplink_decode(wire, RB_ACK_UPLINK_HEADER_SIZE, &out));
	zassert_equal(out.payload_len, 0);
	zassert_ok(rb_ack_uplink_decode(wire, len, &out));
	zassert_equal(out.payload_len, sizeof(bytes));
	zassert_mem_equal(out.payload, bytes, sizeof(bytes));
	zassert_equal(rb_ack_uplink_decode(wire, RB_ACK_UPLINK_HEADER_SIZE - 1,
					   &out), -EMSGSIZE);
	zassert_equal(rb_ack_uplink_decode(wire, sizeof(wire), &out), -EMSGSIZE);
	frame.payload = NULL;
	zassert_equal(rb_ack_uplink_encode(&frame, wire, sizeof(wire), &len),
		      -EINVAL);
	frame.payload = oversized;
	frame.payload_len = sizeof(oversized);
	zassert_equal(rb_ack_uplink_encode(&frame, wire, sizeof(wire), &len),
		      -EMSGSIZE);
}

ZTEST_SUITE(link_protocol, NULL, NULL, NULL, NULL, NULL);
