#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "link_protocol.h"

ZTEST(link_protocol, test_common_header_uses_compact_little_endian_layout)
{
	struct rb_common_header in =
		RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 0x11223344u, 2u);
	struct rb_common_header out;
	const uint8_t expected[] = {'R', 'B', 3u, RB_FRAME_ACK_UPLINK,
				    0x44u, 0x33u, 0x22u, 0x11u, 2u, 0u};
	uint8_t wire[RB_COMMON_HEADER_SIZE];

	zassert_ok(rb_common_encode(&in, wire, sizeof(wire)));
	zassert_mem_equal(wire, expected, sizeof(expected));
	zassert_ok(rb_common_decode(wire, sizeof(wire), &out));
	zassert_equal(out.master_session, in.master_session);
	zassert_equal(out.source_node, 2u);
}

ZTEST(link_protocol, test_common_decode_rejects_bad_magic_version_and_session)
{
	struct rb_common_header header =
		RB_COMMON_INIT(RB_FRAME_SYNC, 1u, 0u);
	struct rb_common_header out;
	uint8_t wire[RB_COMMON_HEADER_SIZE];

	zassert_ok(rb_common_encode(&header, wire, sizeof(wire)));
	wire[0] = 'X';
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EBADMSG);
	wire[0] = 'R';
	wire[2] = 2u;
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EBADMSG);
	wire[2] = RB_PROTOCOL_VERSION;
	wire[4] = 0u;
	wire[5] = 0u;
	wire[6] = 0u;
	wire[7] = 0u;
	zassert_equal(rb_common_decode(wire, sizeof(wire), &out), -EINVAL);
}

ZTEST(link_protocol, test_sync_round_trip_has_no_discovery_fields)
{
	struct rb_sync_frame in = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC, 0x11223344u, 0u),
		.group_id = 41u,
		.master_id = 0x0123456789abcdefULL,
		.sync_sequence = 8u,
		.previous_master_address_tick = 123456789ULL,
		.next_pps_master_tick = 124000000ULL,
		.sync_interval_us = 100000u,
		.next_pps_utc_seconds = -1,
		.time_quality = RB_TIME_LOCKED,
	};
	struct rb_sync_frame out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_sync_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_SYNC_WIRE_SIZE);
	zassert_equal(len, 55u);
	zassert_ok(rb_sync_decode(wire, len, &out));
	zassert_equal(out.group_id, in.group_id);
	zassert_equal(out.master_id, in.master_id);
	zassert_equal(out.previous_master_address_tick,
		      in.previous_master_address_tick);
	zassert_equal(out.next_pps_utc_seconds, -1);
	zassert_equal(out.time_quality, RB_TIME_LOCKED);
}

ZTEST(link_protocol, test_sync_rejects_group_zero_bad_quality_and_v2)
{
	struct rb_sync_frame frame = {
		.common = RB_COMMON_INIT(RB_FRAME_SYNC, 1u, 0u),
		.group_id = 1u,
		.master_id = 1u,
		.sync_sequence = 1u,
		.next_pps_master_tick = 2u,
		.sync_interval_us = 100000u,
	};
	struct rb_sync_frame out;
	uint8_t wire[RB_SYNC_WIRE_SIZE];
	size_t len;

	frame.group_id = 0u;
	zassert_equal(rb_sync_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.group_id = 1u;
	frame.time_quality = 99u;
	zassert_equal(rb_sync_encode(&frame, wire, sizeof(wire), &len), -EINVAL);
	frame.time_quality = RB_TIME_UTC_INVALID;
	zassert_ok(rb_sync_encode(&frame, wire, sizeof(wire), &len));
	wire[2] = 2u;
	zassert_equal(rb_sync_decode(wire, len, &out), -EBADMSG);
}

ZTEST(link_protocol, test_downlink_keeps_opaque_bytes)
{
	const uint8_t bytes[] = {0x00u, 0xfdu, 0x7eu, 0xffu, 0x00u};
	struct rb_downlink_frame out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_downlink_encode(7u, 12u, bytes, sizeof(bytes), wire,
				      sizeof(wire), &len));
	zassert_ok(rb_downlink_decode(wire, len, &out));
	zassert_equal(out.common.master_session, 7u);
	zassert_equal(out.sequence, 12u);
	zassert_mem_equal(out.payload, bytes, sizeof(bytes));
}

ZTEST(link_protocol, test_downlink_rejects_empty_zero_sequence_and_oversize)
{
	uint8_t wire[RB_ESB_MAX_PAYLOAD + 1u];
	uint8_t payload[RB_PACKET_DATA_MAX + 1u] = {0};
	size_t len;

	zassert_equal(rb_downlink_encode(1u, 0u, payload, 1u, wire,
					 sizeof(wire), &len), -EINVAL);
	zassert_equal(rb_downlink_encode(1u, 1u, payload, 0u, wire,
					 sizeof(wire), &len), -EINVAL);
	zassert_equal(rb_downlink_encode(1u, 1u, payload, sizeof(payload), wire,
					 sizeof(wire), &len), -EINVAL);
}

ZTEST(link_protocol, test_poll_round_trip_uses_fixed_slave_session)
{
	struct rb_poll in = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 0x10203040u, 0u),
		.known_slave_session = 0x55667788u,
		.uplink_ack_sequence = 17u,
		.next_credit_bytes = 128u,
		.poll_sequence = 9u,
	};
	struct rb_poll out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_poll_encode(&in, wire, sizeof(wire), &len));
	zassert_equal(len, RB_POLL_WIRE_SIZE);
	zassert_ok(rb_poll_decode(wire, len, &out));
	zassert_equal(out.known_slave_session, in.known_slave_session);
	zassert_equal(out.uplink_ack_sequence, in.uplink_ack_sequence);
	zassert_equal(out.poll_sequence, 9u);
}

ZTEST(link_protocol, test_poll_rejects_excess_credit_and_wrong_source)
{
	struct rb_poll poll = {
		.common = RB_COMMON_INIT(RB_FRAME_POLL, 1u, 0u),
		.next_credit_bytes = RB_ACK_UPLINK_PAYLOAD_MAX + 1u,
	};
	uint8_t wire[RB_POLL_WIRE_SIZE];
	size_t len;

	zassert_equal(rb_poll_encode(&poll, wire, sizeof(wire), &len), -EINVAL);
	poll.next_credit_bytes = 1u;
	poll.common.source_node = 1u;
	zassert_equal(rb_poll_encode(&poll, wire, sizeof(wire), &len), -EINVAL);
}

ZTEST(link_protocol, test_ack_identifies_fixed_node_and_slave_session)
{
	const uint8_t payload[] = {0x11u, 0x22u};
	struct rb_ack_uplink in = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7u, 2u),
		.slave_session = 0xaabbccddu,
		.uplink_sequence = 3u,
		.drop_count = 4u,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	struct rb_ack_uplink out;
	uint8_t wire[RB_ESB_MAX_PAYLOAD];
	size_t len;

	zassert_ok(rb_ack_uplink_encode(&in, wire, sizeof(wire), &len));
	zassert_ok(rb_ack_uplink_decode(wire, len, &out));
	zassert_equal(out.common.source_node, 2u);
	zassert_equal(out.slave_session, in.slave_session);
	zassert_equal(out.drop_count, 4u);
	zassert_mem_equal(out.payload, payload, sizeof(payload));
}

ZTEST(link_protocol, test_empty_ack_requires_zero_sequence_and_valid_node)
{
	struct rb_ack_uplink ack = {
		.common = RB_COMMON_INIT(RB_FRAME_ACK_UPLINK, 7u, 1u),
		.slave_session = 1u,
	};
	uint8_t wire[RB_ACK_UPLINK_HEADER_SIZE];
	size_t len;

	zassert_ok(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len));
	ack.uplink_sequence = 1u;
	zassert_equal(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len), -EINVAL);
	ack.uplink_sequence = 0u;
	ack.common.source_node = 0u;
	zassert_equal(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len), -EINVAL);
	ack.common.source_node = 1u;
	ack.slave_session = 0u;
	zassert_equal(rb_ack_uplink_encode(&ack, wire, sizeof(wire), &len), -EINVAL);
}

ZTEST_SUITE(link_protocol, NULL, NULL, NULL, NULL, NULL);
