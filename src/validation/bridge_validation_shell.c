#include <errno.h>
#include <stdlib.h>

#include <zephyr/shell/shell.h>

#include "bridge_runtime.h"
#include "pps_input.h"
#include "radio_transport.h"
#include "time_uart.h"
#include "uart_bridge.h"

static int parse_uint(const char *text, unsigned long maximum,
		      unsigned long *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
		return -EINVAL;
	}
	*value = parsed;
	return 0;
}

static int parse_utc_seconds(const char *text, int64_t *value)
{
	char *end;
	long long parsed;

	errno = 0;
	parsed = strtoll(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed < 0) {
		return -EINVAL;
	}
	*value = (int64_t)parsed;
	return 0;
}

static int cmd_bridge_test_inject(const struct shell *shell, size_t argc,
				  char **argv)
{
	unsigned long len;
	unsigned long seed;
	int ret;

	if (argc != 3u ||
	    parse_uint(argv[1], RB_VALIDATION_BUFFER_SIZE, &len) != 0 ||
	    parse_uint(argv[2], UINT8_MAX, &seed) != 0 || len == 0u) {
		shell_error(shell, "usage: bridge_test inject <1..%u> <0..255>",
			    RB_VALIDATION_BUFFER_SIZE);
		return -EINVAL;
	}
	ret = bridge_runtime_validation_inject((size_t)len, (uint8_t)seed);
	if (ret != 0) {
		shell_error(shell, "bridge_test inject failed err=%d", ret);
		return ret;
	}
	shell_print(shell, "bridge_test inject ok len=%lu seed=%lu", len, seed);
	return 0;
}

static int cmd_bridge_test_inject_uart(const struct shell *shell, size_t argc,
				       char **argv)
{
	unsigned long len;
	unsigned long seed;
	int ret;

	if (argc != 3u ||
	    parse_uint(argv[1], RB_VALIDATION_BUFFER_SIZE, &len) != 0 ||
	    parse_uint(argv[2], UINT8_MAX, &seed) != 0 || len == 0u) {
		shell_error(shell, "usage: bridge_test inject_uart <len> <seed>");
		return -EINVAL;
	}
	ret = bridge_runtime_validation_inject_uart((size_t)len, (uint8_t)seed);
	if (ret != 0) {
		shell_error(shell, "bridge_test inject_uart failed err=%d", ret);
		return ret;
	}
	shell_print(shell, "bridge_test inject_uart ok len=%lu seed=%lu", len, seed);
	return 0;
}

static int cmd_bridge_test_verify(const struct shell *shell, size_t argc,
				  char **argv)
{
	struct rb_validation_stats stats;
	unsigned long len;
	unsigned long seed;
	size_t mismatch = 0u;
	int ret;

	if (argc != 3u ||
	    parse_uint(argv[1], RB_VALIDATION_BUFFER_SIZE, &len) != 0 ||
	    parse_uint(argv[2], UINT8_MAX, &seed) != 0 || len == 0u) {
		shell_error(shell, "usage: bridge_test verify <1..%u> <0..255>",
			    RB_VALIDATION_BUFFER_SIZE);
		return -EINVAL;
	}
	ret = bridge_runtime_validation_verify((size_t)len, (uint8_t)seed,
					       &mismatch);
	if (ret == -EAGAIN) {
		bridge_runtime_validation_stats_get(&stats);
		shell_error(shell, "bridge_test verify pending need=%lu available=%zu",
			    len, stats.output_bytes);
		return ret;
	}
	if (ret == -EOVERFLOW) {
		bridge_runtime_validation_stats_get(&stats);
		shell_error(shell, "bridge_test verify overflow dropped=%llu",
			    (unsigned long long)stats.output_drop_bytes);
		return ret;
	}
	if (ret != 0) {
		shell_error(shell, "bridge_test verify failed err=%d offset=%zu",
			    ret, mismatch);
		return ret;
	}
	shell_print(shell, "bridge_test verify ok len=%lu seed=%lu", len, seed);
	return 0;
}

static int cmd_bridge_test_verify_pair(const struct shell *shell, size_t argc,
				       char **argv)
{
	unsigned long first_len;
	unsigned long first_seed;
	unsigned long second_len;
	unsigned long second_seed;
	int ret;

	if (argc != 5u ||
	    parse_uint(argv[1], RB_VALIDATION_BUFFER_SIZE, &first_len) != 0 ||
	    parse_uint(argv[2], UINT8_MAX, &first_seed) != 0 ||
	    parse_uint(argv[3], RB_VALIDATION_BUFFER_SIZE, &second_len) != 0 ||
	    parse_uint(argv[4], UINT8_MAX, &second_seed) != 0 ||
	    first_len == 0u || second_len == 0u ||
	    first_len + second_len > RB_VALIDATION_BUFFER_SIZE) {
		shell_error(shell,
			    "usage: bridge_test verify_pair <len1> <seed1> <len2> <seed2>");
		return -EINVAL;
	}
	ret = bridge_runtime_validation_verify_pair(
		(size_t)first_len, (uint8_t)first_seed,
		(size_t)second_len, (uint8_t)second_seed);
	if (ret == -EAGAIN) {
		shell_error(shell,
			    "bridge_test verify_pair pending need=%lu available=less",
			    first_len + second_len);
		return ret;
	}
	if (ret != 0) {
		shell_error(shell, "bridge_test verify_pair failed err=%d", ret);
		return ret;
	}
	shell_print(shell,
		    "bridge_test verify_pair ok len1=%lu seed1=%lu len2=%lu seed2=%lu",
		    first_len, first_seed, second_len, second_seed);
	return 0;
}

static int cmd_bridge_test_stats(const struct shell *shell, size_t argc,
				 char **argv)
{
	struct rb_validation_stats stats;
	struct rb_bridge_stats bridge;
	const struct rb_uart_stats *uart;
	const struct rb_time_uart_stats *time_uart;
	uint32_t loss_dropped = 0u;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bridge_runtime_validation_stats_get(&stats);
	bridge_runtime_stats_get(&bridge);
	uart = uart_bridge_stats_get();
	time_uart = time_uart_stats_get();
#if defined(CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION)
	loss_dropped = radio_transport_loss_drop_count();
#endif
	shell_print(shell, "bridge_test stats input=%zu output=%zu injected=%llu "
		    "captured=%llu input_drop=%llu output_drop=%llu "
		    "uart_rx_drop_bytes=%llu uart_tx_drop_bytes=%llu "
		    "uart_rx_stopped_events=%u uart_rx_stop_reason_mask=%u "
		    "uart_rx_disabled_events=%u uart_rx_restart_errors=%u "
		    "uart_rx_restart_delay_ms=%u "
		    "queue_drop_bytes=%llu "
		    "radio_event_drop_count=%u "
		    "active_count=%u active_mask=0x%02x "
		    "node1_ack_age_us=%lld node1_poll_failures=%u "
		    "node2_ack_age_us=%lld node2_poll_failures=%u "
		    "node3_ack_age_us=%lld node3_poll_failures=%u "
		    "slave_active=%u slave_node_id=%u slave_session=%u "
		    "invalid_session_packets=%llu invalid_node_packets=%llu "
		    "downlink_gap=%llu downlink_duplicate=%llu "
		    "node1_records=%llu node1_bytes=%llu node1_record_drop=%llu "
		    "node2_records=%llu node2_bytes=%llu node2_record_drop=%llu "
		    "node3_records=%llu node3_bytes=%llu node3_record_drop=%llu "
		    "uart_record_queued=%llu uart_record_completed=%llu "
		    "uart_record_aborted=%llu uart_record_rejected=%llu "
		    "uart_record_pending=%u uart_record_busy=%u uart_start_errors=%u "
		    "loss_dropped=%u",
		    stats.input_bytes, stats.output_bytes,
		    (unsigned long long)stats.injected_bytes,
		    (unsigned long long)stats.captured_bytes,
		    (unsigned long long)stats.input_drop_bytes,
		    (unsigned long long)stats.output_drop_bytes,
		    (unsigned long long)uart->rx_drop_bytes,
		    (unsigned long long)uart->tx_drop_bytes,
		    uart->rx_stopped_events, uart->rx_stop_reason_mask,
		    uart->rx_disabled_events, uart->rx_restart_errors,
		    uart->rx_restart_delay_ms,
		    (unsigned long long)bridge.queue_drop_bytes,
		    bridge.radio_event_drop_count,
		    bridge.active_count, bridge.active_mask,
		    (long long)bridge.node1_ack_age_us, bridge.node1_poll_failures,
		    (long long)bridge.node2_ack_age_us, bridge.node2_poll_failures,
		    (long long)bridge.node3_ack_age_us, bridge.node3_poll_failures,
		    bridge.slave_active, bridge.slave_node_id, bridge.slave_session,
		    (unsigned long long)bridge.invalid_session_packets,
		    (unsigned long long)bridge.invalid_node_packets,
		    (unsigned long long)bridge.downlink_gap_packets,
		    (unsigned long long)bridge.downlink_duplicate_packets,
		    (unsigned long long)bridge.node_record_count[0],
		    (unsigned long long)bridge.node_record_bytes[0],
		    (unsigned long long)bridge.node_record_drop[0],
		    (unsigned long long)bridge.node_record_count[1],
		    (unsigned long long)bridge.node_record_bytes[1],
		    (unsigned long long)bridge.node_record_drop[1],
		    (unsigned long long)bridge.node_record_count[2],
		    (unsigned long long)bridge.node_record_bytes[2],
		    (unsigned long long)bridge.node_record_drop[2],
		    (unsigned long long)uart->tx_record_queued,
		    (unsigned long long)uart->tx_record_completed,
		    (unsigned long long)uart->tx_record_aborted,
		    (unsigned long long)uart->tx_record_rejected,
		    uart->tx_record_pending, uart->tx_record_busy,
		    uart->tx_start_errors, loss_dropped);
	shell_print(shell, "bridge_test time_uart rx_drop_bytes=%llu "
		    "overlong_line_drops=%u output_line_drops=%u "
		    "rx_restart_errors=%u rx_buffer_errors=%u "
		    "rx_stopped_events=%u rx_stop_reason_mask=%u "
		    "pps_input_drop_count=%llu",
		    (unsigned long long)time_uart->rx_drop_bytes,
		    time_uart->overlong_line_drops,
		    time_uart->output_line_drops,
		    time_uart->rx_restart_errors,
		    time_uart->rx_buffer_errors,
		    time_uart->rx_stopped_events,
		    time_uart->rx_stop_reason_mask,
		    (unsigned long long)pps_input_drop_count());
	return 0;
}

#if defined(CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION)
static int cmd_bridge_test_loss(const struct shell *shell, size_t argc,
				char **argv)
{
	unsigned long frame_type;
	unsigned long every_n;
	uint32_t type_mask;

	if (argc != 3u || parse_uint(argv[1], 31u, &frame_type) != 0 ||
	    parse_uint(argv[2], UINT32_MAX, &every_n) != 0 ||
	    frame_type == 0u || every_n == 0u) {
		shell_error(shell, "usage: bridge_test loss <1..31> <1..4294967295>");
		return -EINVAL;
	}
	type_mask = 1u << frame_type;
	radio_transport_loss_set(type_mask, (uint32_t)every_n);
	shell_print(shell, "bridge_test loss ok mask=0x%08x every_n=%lu",
		    type_mask, every_n);
	return 0;
}

static int cmd_bridge_test_loss_off(const struct shell *shell, size_t argc,
				    char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	radio_transport_loss_set(0u, 0u);
	shell_print(shell, "bridge_test loss_off ok mask=0x00000000 every_n=0");
	return 0;
}

static int cmd_bridge_test_loss_once(const struct shell *shell, size_t argc,
				     char **argv)
{
	unsigned long frame_type;
	uint32_t type_mask;

	if (argc != 2u || parse_uint(argv[1], 31u, &frame_type) != 0 ||
	    frame_type == 0u) {
		shell_error(shell, "usage: bridge_test loss_once <1..31>");
		return -EINVAL;
	}
	type_mask = 1u << frame_type;
	radio_transport_loss_once(type_mask,
		frame_type == RB_FRAME_ACK_UPLINK ? RB_ACK_UPLINK_HEADER_SIZE + 1u : 0u);
	shell_print(shell, "bridge_test loss_once ok mask=0x%08x min_len=%u",
		    type_mask,
		    frame_type == RB_FRAME_ACK_UPLINK ?
			RB_ACK_UPLINK_HEADER_SIZE + 1u : 0u);
	return 0;
}
#endif

static int cmd_bridge_test_dump(const struct shell *shell, size_t argc,
				char **argv)
{
	struct rb_validation_stats stats;
	unsigned long requested;
	uint8_t data[32];
	char hex[sizeof(data) * 2u + 1u];
	size_t offset = 0u;

	if (argc != 2u ||
	    parse_uint(argv[1], RB_VALIDATION_BUFFER_SIZE, &requested) != 0 ||
	    requested == 0u) {
		shell_error(shell, "usage: bridge_test dump <1..%u>",
			    RB_VALIDATION_BUFFER_SIZE);
		return -EINVAL;
	}
	bridge_runtime_validation_stats_get(&stats);
	if (stats.output_bytes < requested) {
		shell_error(shell, "bridge_test dump pending need=%lu available=%zu",
			    requested, stats.output_bytes);
		return -EAGAIN;
	}
	while (offset < requested) {
		size_t chunk_len = requested - offset;
		size_t copied;

		if (chunk_len > sizeof(data)) {
			chunk_len = sizeof(data);
		}
		copied = bridge_runtime_validation_copy(offset, data, chunk_len);
		if (copied != chunk_len) {
			return -EIO;
		}
		for (size_t i = 0u; i < copied; i++) {
			static const char digits[] = "0123456789abcdef";

			hex[i * 2u] = digits[data[i] >> 4];
			hex[i * 2u + 1u] = digits[data[i] & 0x0fu];
		}
		hex[copied * 2u] = '\0';
		shell_print(shell, "bridge_test rx offset=%zu data=%s", offset, hex);
		offset += copied;
	}
	shell_print(shell, "bridge_test dump ok len=%lu", requested);
	return 0;
}

static int cmd_bridge_test_clear(const struct shell *shell, size_t argc,
				 char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bridge_runtime_validation_clear();
	shell_print(shell, "bridge_test clear ok");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bridge_test,
	SHELL_CMD_ARG(inject, NULL, "Inject pattern: <length> <seed>",
		      cmd_bridge_test_inject, 3, 0),
	SHELL_CMD_ARG(inject_uart, NULL, "Inject pattern at UART rate: <length> <seed>",
		      cmd_bridge_test_inject_uart, 3, 0),
	SHELL_CMD_ARG(verify, NULL, "Verify received pattern: <length> <seed>",
		      cmd_bridge_test_verify, 3, 0),
	SHELL_CMD_ARG(verify_pair, NULL,
		      "Verify two whole records in either order: <len1> <seed1> <len2> <seed2>",
		      cmd_bridge_test_verify_pair, 5, 0),
	SHELL_CMD_ARG(dump, NULL, "Print received bytes as hex: <length>",
		      cmd_bridge_test_dump, 2, 0),
	SHELL_CMD(stats, NULL, "Show validation queue statistics",
		  cmd_bridge_test_stats),
	SHELL_CMD(clear, NULL, "Clear validation queues and counters",
		  cmd_bridge_test_clear),
#if defined(CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION)
	SHELL_CMD_ARG(loss, NULL, "Drop frame type at cadence: <type> <every_n>",
		      cmd_bridge_test_loss, 3, 0),
	SHELL_CMD_ARG(loss_once, NULL, "Drop the next matching frame: <type>",
		      cmd_bridge_test_loss_once, 2, 0),
	SHELL_CMD(loss_off, NULL, "Disable radio packet loss injection",
		  cmd_bridge_test_loss_off),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bridge_test, &sub_bridge_test,
		   "Validation-only bridge payload commands", NULL);

static int cmd_time_test_pair(const struct shell *shell, size_t argc,
			      char **argv)
{
	int64_t utc_seconds;
	int ret;

	if (argc != 2u || parse_utc_seconds(argv[1], &utc_seconds) != 0) {
		shell_error(shell, "usage: time_test pair <utc_seconds>");
		return -EINVAL;
	}
	ret = bridge_runtime_validation_time_pair(utc_seconds);
	if (ret != 0) {
		shell_error(shell, "time_test pair failed err=%d", ret);
		return ret;
	}
	shell_print(shell, "time_test pair queued utc_seconds=%lld",
		    (long long)utc_seconds);
	return 0;
}

static int cmd_time_test_source_lost(const struct shell *shell, size_t argc,
				     char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = bridge_runtime_validation_time_source_lost();
	if (ret != 0) {
		shell_error(shell, "time_test source_lost failed err=%d", ret);
		return ret;
	}
	shell_print(shell, "time_test source_lost queued");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_time_test,
	SHELL_CMD_ARG(pair, NULL, "Synthesize PPS/NMEA pair: <utc_seconds>",
		      cmd_time_test_pair, 2, 0),
	SHELL_CMD(source_lost, NULL, "Age external PPS/NMEA inputs by 3 seconds",
		  cmd_time_test_source_lost),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(time_test, &sub_time_test,
		   "Validation-only external time-source commands", NULL);
