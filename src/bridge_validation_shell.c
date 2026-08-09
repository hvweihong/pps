#include <errno.h>
#include <stdlib.h>

#include <zephyr/shell/shell.h>

#include "bridge_runtime.h"

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
	if (ret != 0) {
		shell_error(shell, "bridge_test verify failed err=%d offset=%zu",
			    ret, mismatch);
		return ret;
	}
	shell_print(shell, "bridge_test verify ok len=%lu seed=%lu", len, seed);
	return 0;
}

static int cmd_bridge_test_stats(const struct shell *shell, size_t argc,
				 char **argv)
{
	struct rb_validation_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bridge_runtime_validation_stats_get(&stats);
	shell_print(shell, "bridge_test stats input=%zu output=%zu injected=%llu "
		    "captured=%llu input_drop=%llu output_drop=%llu",
		    stats.input_bytes, stats.output_bytes,
		    (unsigned long long)stats.injected_bytes,
		    (unsigned long long)stats.captured_bytes,
		    (unsigned long long)stats.input_drop_bytes,
		    (unsigned long long)stats.output_drop_bytes);
	return 0;
}

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
	SHELL_CMD_ARG(verify, NULL, "Verify received pattern: <length> <seed>",
		      cmd_bridge_test_verify, 3, 0),
	SHELL_CMD_ARG(dump, NULL, "Print received bytes as hex: <length>",
		      cmd_bridge_test_dump, 2, 0),
	SHELL_CMD(stats, NULL, "Show validation queue statistics",
		  cmd_bridge_test_stats),
	SHELL_CMD(clear, NULL, "Clear validation queues and counters",
		  cmd_bridge_test_clear),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bridge_test, &sub_bridge_test,
		   "Validation-only bridge payload commands", NULL);
