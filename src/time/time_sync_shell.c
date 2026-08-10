/*
 * Time Sync Shell Commands
 *
 * Shell interface for time synchronization status and control.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <stdio.h>

#include "sync_filter.h"
#include "bridge_runtime.h"
#include "timebase.h"
#include "utc_clock.h"

static const char *utc_quality_to_string(uint8_t quality)
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

static const char *time_source_to_string(const struct rb_bridge_stats *stats)
{
	if (!stats->is_master) {
		return "WIRELESS";
	}
	if (stats->time_source_mode == 0u) {
		return "LOCAL";
	}
	switch (stats->utc_state) {
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

/* Helper: format sync state as string */
static const char *state_to_string(enum sync_filter_state state)
{
	switch (state) {
	case SYNC_FILTER_UNLOCKED:
		return "UNLOCKED";
	case SYNC_FILTER_ACQUIRING:
		return "ACQUIRING";
	case SYNC_FILTER_LOCKED:
		return "LOCKED";
	case SYNC_FILTER_HOLDOVER:
		return "HOLDOVER";
	default:
		return "UNKNOWN";
	}
}

/* time_sync status - Display synchronization status */
static int cmd_time_sync_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rb_bridge_stats stats;
	enum sync_filter_state state;
	int64_t offset_us;
	int32_t drift_ppm;
	uint64_t local_time;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Get current state */
	bridge_runtime_stats_get(&stats);
	state = bridge_runtime_sync_state();
	offset_us = bridge_runtime_sync_offset_us();
	drift_ppm = bridge_runtime_sync_drift_ppm();
	local_time = timebase_now_us();

	shell_print(sh, "Time Synchronization Status:");
	shell_print(sh, "  State:           %s", state_to_string(state));
	shell_print(sh, "  Time Source:     %s", time_source_to_string(&stats));
	shell_print(sh, "  UTC Quality:     %s",
		    utc_quality_to_string(stats.utc_quality));
	shell_print(sh, "  UTC Seconds:     %lld", (long long)stats.utc_seconds);
	shell_print(sh, "  Local Time:      %llu us", local_time);
	shell_print(sh, "  Offset:          %lld us", offset_us);
	shell_print(sh, "  Drift:           %d ppm", (int)drift_ppm);
	shell_print(sh, "  Jitter:          %u us", stats.sync_jitter);
	shell_print(sh, "  RX Count:        %llu", stats.sync_rx_count);
	shell_print(sh, "  Missed Count:    %llu", stats.sync_missed_count);
	shell_print(sh, "  Sync Age:        %u us", stats.sync_age_us);

	if (state == SYNC_FILTER_LOCKED) {
		shell_print(sh, "  Status:          Synchronized");
	} else if (state == SYNC_FILTER_ACQUIRING) {
		shell_print(sh, "  Status:          Acquiring lock...");
	} else if (state == SYNC_FILTER_HOLDOVER) {
		shell_print(sh, "  Status:          Holdover (lost sync)");
	} else {
		shell_print(sh, "  Status:          Not synchronized");
	}

	return 0;
}

/* time_sync stats - Display detailed statistics */
static int cmd_time_sync_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct rb_bridge_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bridge_runtime_stats_get(&stats);

	shell_print(sh, "Time Sync Statistics:");
	shell_print(sh, "  Sync RX:         %llu", stats.sync_rx_count);
	shell_print(sh, "  Sync Missed:     %llu", stats.sync_missed_count);
	shell_print(sh, "  Current Offset:  %lld us", stats.sync_offset);
	shell_print(sh, "  Current Jitter:  %u us", stats.sync_jitter);
	shell_print(sh, "  External PPS:    %llu", stats.external_pps_count);
	shell_print(sh, "  NMEA Valid:      %llu", stats.nmea_valid_count);
	shell_print(sh, "  NMEA Drops:      %llu", stats.nmea_drop_count);
	shell_print(sh, "  Holdover Count:  %llu", stats.holdover_count);

	if (stats.sync_rx_count > 0) {
		uint64_t total = stats.sync_rx_count + stats.sync_missed_count;
		uint32_t success_rate = (uint32_t)((stats.sync_rx_count * 100) / total);
		shell_print(sh, "  Success Rate:    %u%%", success_rate);
	}

	return 0;
}

/* Define time_sync subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_time_sync,
	SHELL_CMD(status, NULL, "Show time synchronization status", cmd_time_sync_status),
	SHELL_CMD(stats, NULL, "Show time synchronization statistics", cmd_time_sync_stats),
	SHELL_SUBCMD_SET_END
);

/* Register time_sync command */
SHELL_CMD_REGISTER(time_sync, &sub_time_sync, "Time synchronization control", NULL);
