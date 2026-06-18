#include "app_config.h"
#include "ble_time_sync.h"
#include "ble_time_sync_service.h"
#include "heartbeat_led.h"
#include "pps_output.h"
#include "radio_sync.h"
#include "runtime_config.h"
#include "status.h"
#include "sync_filter.h"
#include "sync_packet.h"
#include "time_sync_math.h"
#include "timebase.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(time_sync_app, LOG_LEVEL_INF);

#define STATUS_SLEEP_MS 10
#define MASTER_BLE_START_SUCCESS_BEACONS 30u
#define MASTER_BLE_ERROR_RETRY_BEACONS 10u
#define SLAVE_PPS_KEEP_PENDING_WINDOW_US 20000u
#define SLAVE_PPS_MIN_ARM_AHEAD_US 1000u
#define SLAVE_RX_MIN_START_AHEAD_US 1000u
#define SLAVE_RX_REQUEST_GUARD_US 20000u
#define SLAVE_RX_ACQUIRE_RETRY_GAP_US TIME_SYNC_RX_ACQUIRE_PERIOD_US_VALUE
#define SLAVE_BLE_START_LOCKED_BEACONS 20u
#define SLAVE_SLEEP_MS 1

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static void master_loop(void)
{
	uint64_t now = timebase_now_us();
	uint64_t first_txen = now + TIME_SYNC_INTERVAL_US_VALUE;
	uint64_t next_pps = time_sync_next_period_tick(first_txen,
						       TIME_SYNC_PPS_PERIOD_US_VALUE);
	uint64_t next_status = now;
	uint32_t stable_tx_count = 0;
	uint32_t last_tx_packets = 0;
	uint32_t last_tx_error_count = 0;
	int ret;

	LOG_INF("role=master network=0x%08x rf_channel=%u interval=%uus "
		"txen_to_address=%uus tx_slot=%uus tx_offset=%uus",
		TIME_SYNC_NETWORK_ID_VALUE, TIME_SYNC_RF_CHANNEL_VALUE,
		TIME_SYNC_INTERVAL_US_VALUE,
		TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US_VALUE,
		TIME_SYNC_MASTER_TX_TIMESLOT_LENGTH_US_VALUE,
		TIME_SYNC_MASTER_TX_START_OFFSET_US_VALUE);

	ret = pps_output_start_periodic(next_pps,
					TIME_SYNC_PPS_PERIOD_US_VALUE);
	if (ret != 0) {
		LOG_ERR("master pps start failed: %d", ret);
		return;
	}

	struct radio_sync_master_config master_cfg = {
		.initial_seq = 0,
		.network_id = TIME_SYNC_NETWORK_ID_VALUE,
		.sync_interval_us = TIME_SYNC_INTERVAL_US_VALUE,
		.pps_period_us = TIME_SYNC_PPS_PERIOD_US_VALUE,
		.pps_epoch_tick = next_pps,
		.txen_to_address_us = TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US_VALUE,
	};

	ret = radio_sync_start_master(&master_cfg, first_txen);
	if (ret != 0) {
		LOG_ERR("master radio start failed: %d", ret);
		return;
	}

	while (true) {
		now = timebase_now_us();
		const struct radio_sync_stats *radio_stats = radio_sync_stats_get();
		uint32_t tx_error_count = radio_stats->tx_mpsl_blocked +
			radio_stats->tx_mpsl_cancelled +
			radio_stats->tx_timeout_errors +
			radio_stats->tx_prepare_late +
			radio_stats->tx_mpsl_request_errors;
		bool tx_progress = radio_stats->tx_packets != last_tx_packets;

		if (tx_error_count != last_tx_error_count) {
			last_tx_error_count = tx_error_count;
			stable_tx_count = 0;
		} else if (tx_progress) {
			uint32_t delta = radio_stats->tx_packets - last_tx_packets;

			last_tx_packets = radio_stats->tx_packets;
			if (stable_tx_count + delta < MASTER_BLE_START_SUCCESS_BEACONS) {
				stable_tx_count += delta;
			} else {
				stable_tx_count = MASTER_BLE_START_SUCCESS_BEACONS;
			}
		}

#if defined(CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START)
		if (!ble_time_sync_started() &&
		    stable_tx_count >= MASTER_BLE_START_SUCCESS_BEACONS) {
			ret = ble_time_sync_start();
			if (ret == 0) {
				LOG_INF("BLE start after radio tx stable seq=%u",
					radio_stats->tx_sequence);
			} else {
				stable_tx_count =
					MASTER_BLE_START_SUCCESS_BEACONS -
					MASTER_BLE_ERROR_RETRY_BEACONS;
				LOG_WRN("BLE start failed: %d", ret);
			}
		}
#endif

		if ((int64_t)(now - next_status) >= 0) {
			struct status_snapshot snapshot = {
				.role = "master",
				.seq = (uint16_t)radio_stats->tx_sequence,
				.filter_state = SYNC_FILTER_LOCKED,
				.offset_us = 0,
				.drift_ppm = 0,
				.missed_beacons = 0,
				.next_pps_tick =
					pps_output_stats_get()->scheduled_rise_tick,
				.last_beacon_age_us = 0,
				.ble_gate_count = stable_tx_count,
				.ble_connected = ble_time_sync_connected(),
			};

			ble_time_sync_get_snapshot(&snapshot.ble);
			status_log(&snapshot);
			next_status = now + TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000ULL;
		}

		k_sleep(K_MSEC(STATUS_SLEEP_MS));
	}
}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
static uint32_t slave_rx_window_length_us(enum radio_sync_rx_mode rx_mode)
{
	uint32_t base_window_len;
	uint32_t max_window_len;

	switch (rx_mode) {
	case RADIO_SYNC_RX_LOCKED_WINDOW:
		base_window_len = TIME_SYNC_RX_LOCKED_PRE_MARGIN_US_VALUE +
			TIME_SYNC_RX_LOCKED_POST_MARGIN_US_VALUE +
			TIME_SYNC_RX_WINDOW_GUARD_US_VALUE;
		break;
	case RADIO_SYNC_RX_RECOVERY_WINDOW:
		base_window_len = TIME_SYNC_RX_RECOVERY_PRE_MARGIN_US_VALUE +
			TIME_SYNC_RX_RECOVERY_POST_MARGIN_US_VALUE +
			TIME_SYNC_RX_WINDOW_GUARD_US_VALUE;
		break;
	case RADIO_SYNC_RX_ACQUIRE:
	default:
		if (ble_time_sync_started()) {
			base_window_len = TIME_SYNC_RX_RECOVERY_PRE_MARGIN_US_VALUE +
				TIME_SYNC_RX_RECOVERY_POST_MARGIN_US_VALUE +
				TIME_SYNC_RX_WINDOW_GUARD_US_VALUE;
		} else {
			base_window_len = TIME_SYNC_RX_ACQUIRE_WINDOW_US_VALUE;
		}
		break;
	}

	max_window_len = TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
	if (max_window_len > TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE +
	     TIME_SYNC_RX_WINDOW_GUARD_US_VALUE +
	     TIME_SYNC_RX_START_OFFSET_US_VALUE) {
		max_window_len -= TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE +
				  TIME_SYNC_RX_WINDOW_GUARD_US_VALUE +
				  TIME_SYNC_RX_START_OFFSET_US_VALUE;
	} else {
		max_window_len = TIME_SYNC_TIMESLOT_LENGTH_US_VALUE / 2u;
	}

	if (base_window_len > max_window_len) {
		return max_window_len;
	}

	return base_window_len;
}

static uint32_t slave_rx_pre_margin_us(enum radio_sync_rx_mode rx_mode)
{
	if (rx_mode == RADIO_SYNC_RX_RECOVERY_WINDOW) {
		return TIME_SYNC_RX_RECOVERY_PRE_MARGIN_US_VALUE;
	}

	return TIME_SYNC_RX_LOCKED_PRE_MARGIN_US_VALUE;
}

static uint32_t slave_rx_retry_gap_us(void)
{
	return SLAVE_RX_ACQUIRE_RETRY_GAP_US;
}

static uint32_t slave_rx_request_ahead_us(uint32_t sync_interval_us)
{
	if (sync_interval_us > SLAVE_RX_REQUEST_GUARD_US) {
		return sync_interval_us - SLAVE_RX_REQUEST_GUARD_US;
	}

	return sync_interval_us / 2u;
}

static int slave_predict_master_to_local(const struct sync_filter *filter,
					 uint64_t master_tick,
					 uint64_t *local_tick)
{
	int64_t local;

	if (filter == NULL || local_tick == NULL) {
		return -EINVAL;
	}

	if (sync_filter_state(filter) == SYNC_FILTER_LOCKED ||
	    sync_filter_state(filter) == SYNC_FILTER_HOLDOVER) {
		return sync_filter_master_to_local(filter, master_tick,
						   local_tick);
	}

	if (sync_filter_state(filter) != SYNC_FILTER_ACQUIRING ||
	    filter->valid_count == 0u) {
		return -EAGAIN;
	}

	local = (int64_t)master_tick - filter->offset_us;
	if (local < 0) {
		return -ERANGE;
	}

	*local_tick = (uint64_t)local;
	return 0;
}

static int slave_plan_predicted_window(const struct sync_filter *filter,
				       uint64_t master_tx_tick,
				       enum radio_sync_rx_mode mode,
				       uint64_t *next_rx_window)
{
	uint64_t predicted_local;
	uint32_t pre_margin;
	int ret;

	if (mode != RADIO_SYNC_RX_LOCKED_WINDOW &&
	    mode != RADIO_SYNC_RX_RECOVERY_WINDOW) {
		return -EINVAL;
	}

	ret = slave_predict_master_to_local(filter, master_tx_tick,
					    &predicted_local);
	if (ret != 0) {
		return ret;
	}

	pre_margin = slave_rx_pre_margin_us(mode);
	if (predicted_local <= pre_margin + SLAVE_RX_MIN_START_AHEAD_US) {
		return -ETIME;
	}

	*next_rx_window = predicted_local - pre_margin;
	return 0;
}

static void slave_note_missed_rx_window(
	struct sync_filter *filter, const struct sync_filter_config *cfg,
	const struct time_sync_runtime_config *runtime_cfg,
	enum radio_sync_rx_mode *rx_mode, uint64_t *next_rx_window,
	uint64_t *next_expected_master_tx, uint64_t *last_rx_tick,
	uint64_t *missed_accounted_tick, uint32_t *consecutive_missed_windows,
	uint32_t *locked_beacon_count)
{
	if (filter == NULL || cfg == NULL || runtime_cfg == NULL ||
	    rx_mode == NULL || next_rx_window == NULL ||
	    next_expected_master_tx == NULL || last_rx_tick == NULL ||
	    missed_accounted_tick == NULL ||
	    consecutive_missed_windows == NULL ||
	    locked_beacon_count == NULL) {
		return;
	}

	if (*rx_mode == RADIO_SYNC_RX_LOCKED_WINDOW ||
	    *rx_mode == RADIO_SYNC_RX_RECOVERY_WINDOW) {
		(*consecutive_missed_windows)++;
		*locked_beacon_count = 0;

		if (*consecutive_missed_windows >=
		    TIME_SYNC_RX_MISSED_TO_ACQUIRE_VALUE) {
			sync_filter_init(filter, cfg);
			*last_rx_tick = 0;
			*missed_accounted_tick = 0;
			*next_expected_master_tx = 0;
			*rx_mode = RADIO_SYNC_RX_ACQUIRE;
			*next_rx_window = timebase_now_us() +
				slave_rx_retry_gap_us();
			return;
		}

		if (*next_expected_master_tx != 0u) {
			*next_expected_master_tx += runtime_cfg->sync_interval_us;
			*rx_mode = RADIO_SYNC_RX_RECOVERY_WINDOW;
			if (slave_plan_predicted_window(
				    filter, *next_expected_master_tx,
				    *rx_mode, next_rx_window) == 0) {
				return;
			}
		}

		*next_expected_master_tx = 0;
	}

	*rx_mode = RADIO_SYNC_RX_ACQUIRE;
	*next_rx_window = timebase_now_us() + slave_rx_retry_gap_us();
}

static void reset_slave_sync_state(struct sync_filter *filter,
				   struct sync_filter_config *filter_cfg,
				   const struct time_sync_runtime_config *runtime_cfg,
				   uint16_t *seq, uint64_t *last_rx_tick,
				   uint64_t *missed_accounted_tick,
				   uint64_t *next_expected_master_tx,
				   enum radio_sync_rx_mode *rx_mode,
				   uint64_t *next_rx_window,
				   uint64_t *expected_rx_deadline,
				   uint32_t *consecutive_missed_windows,
				   uint32_t *locked_beacon_count,
				   bool *rx_window_pending)
{
	filter_cfg->sync_interval_us = runtime_cfg->sync_interval_us;
	sync_filter_init(filter, filter_cfg);
	*seq = 0;
	*last_rx_tick = 0;
	*missed_accounted_tick = 0;
	*next_expected_master_tx = 0;
	*rx_mode = RADIO_SYNC_RX_ACQUIRE;
	*next_rx_window = timebase_now_us() + 2000u;
	*expected_rx_deadline = 0;
	*consecutive_missed_windows = 0;
	*locked_beacon_count = 0;
	*rx_window_pending = false;
}

static void slave_notifyf(const char *fmt, ...)
{
	char msg[128];
	va_list args;

	va_start(args, fmt);
	vsnprintk(msg, sizeof(msg), fmt, args);
	va_end(args);

	(void)ble_time_sync_service_notify(msg);
}

static void handle_slave_ble_command(const struct ble_time_sync_command *cmd,
				     struct sync_filter *filter,
				     struct sync_filter_config *filter_cfg,
				     struct time_sync_runtime_config *runtime_cfg,
				     uint16_t *seq, uint64_t *last_rx_tick,
				     uint64_t *missed_accounted_tick,
				     uint64_t *next_expected_master_tx,
				     enum radio_sync_rx_mode *rx_mode,
				     uint64_t *next_rx_window,
				     uint64_t *expected_rx_deadline,
				     uint32_t *consecutive_missed_windows,
				     uint32_t *locked_beacon_count,
				     bool *rx_window_pending)
{
	struct time_sync_runtime_config new_cfg;
	unsigned int network;
	unsigned int channel;
	unsigned int interval;
	int ret;

	if (cmd == NULL) {
		return;
	}

	if (strcmp(cmd->text, "GET_STATUS") == 0) {
		slave_notifyf("OK %s", ble_time_sync_service_status());
		return;
	}

	if (strcmp(cmd->text, "HELLO_FROM_MASTER hello world") == 0) {
		LOG_INF("BLE hello from master: hello world");
		slave_notifyf("HELLO_FROM_SLAVE hello world");
		return;
	}

	if (strcmp(cmd->text, "START_SYNC") == 0) {
		if (sync_filter_state(filter) != SYNC_FILTER_UNLOCKED &&
		    *rx_mode != RADIO_SYNC_RX_STOPPED) {
			slave_notifyf("OK START_SYNC already running network=0x%08x channel=%u interval=%u",
				      runtime_cfg->network_id,
				      runtime_cfg->rf_channel,
				      runtime_cfg->sync_interval_us);
			return;
		}

		ret = radio_sync_reconfigure(runtime_cfg->rf_channel,
					     runtime_cfg->network_id);
		if (ret == 0) {
			reset_slave_sync_state(filter, filter_cfg, runtime_cfg,
					       seq, last_rx_tick,
					       missed_accounted_tick,
					       next_expected_master_tx,
					       rx_mode, next_rx_window,
					       expected_rx_deadline,
					       consecutive_missed_windows,
					       locked_beacon_count,
					       rx_window_pending);
			slave_notifyf("OK START_SYNC network=0x%08x channel=%u interval=%u",
				      runtime_cfg->network_id,
				      runtime_cfg->rf_channel,
				      runtime_cfg->sync_interval_us);
		} else {
			slave_notifyf("ERR START_SYNC %d", ret);
		}
		return;
	}

	if (strcmp(cmd->text, "STOP_SYNC") == 0) {
		ret = radio_sync_stop_rx();
		*rx_mode = RADIO_SYNC_RX_STOPPED;
		*rx_window_pending = false;
		if (ret == 0) {
			slave_notifyf("OK STOP_SYNC");
		} else {
			slave_notifyf("ERR STOP_SYNC %d", ret);
		}
		return;
	}

	if (sscanf(cmd->text, "SET_GROUP network=%x channel=%u interval=%u",
		   &network, &channel, &interval) == 3) {
		if (channel > UINT8_MAX) {
			slave_notifyf("ERR SET_GROUP %d", -EINVAL);
			return;
		}

		new_cfg.network_id = network;
		new_cfg.rf_channel = (uint8_t)channel;
		new_cfg.sync_interval_us = interval;

		if (new_cfg.network_id == runtime_cfg->network_id &&
		    new_cfg.rf_channel == runtime_cfg->rf_channel &&
		    new_cfg.sync_interval_us == runtime_cfg->sync_interval_us) {
			slave_notifyf("OK SET_GROUP unchanged network=0x%08x channel=%u interval=%u",
				      runtime_cfg->network_id,
				      runtime_cfg->rf_channel,
				      runtime_cfg->sync_interval_us);
			return;
		}

		ret = runtime_config_set(&new_cfg);
		if (ret == 0) {
			*runtime_cfg = runtime_config_get();
			ret = radio_sync_reconfigure(runtime_cfg->rf_channel,
						     runtime_cfg->network_id);
		}

		if (ret == 0) {
			reset_slave_sync_state(filter, filter_cfg, runtime_cfg,
					       seq, last_rx_tick,
					       missed_accounted_tick,
					       next_expected_master_tx,
					       rx_mode, next_rx_window,
					       expected_rx_deadline,
					       consecutive_missed_windows,
					       locked_beacon_count,
					       rx_window_pending);
			slave_notifyf("OK SET_GROUP network=0x%08x channel=%u interval=%u",
				      runtime_cfg->network_id,
				      runtime_cfg->rf_channel,
				      runtime_cfg->sync_interval_us);
		} else {
			slave_notifyf("ERR SET_GROUP %d", ret);
		}
		return;
	}

	slave_notifyf("ERR UNKNOWN_CMD");
}

static void slave_loop(void)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	struct time_sync_runtime_config runtime_cfg = runtime_config_get();
	uint16_t seq = 0;
	uint64_t last_rx_tick = 0;
	uint64_t missed_accounted_tick = 0;
	uint64_t next_expected_master_tx = 0;
	uint64_t now = timebase_now_us();
	uint64_t next_status = now;
	uint64_t next_rx_window = now + 2000u;
	uint64_t expected_rx_deadline = 0;
	uint64_t free_run_pps = time_sync_next_period_tick(
		now + 10000u, TIME_SYNC_PPS_PERIOD_US_VALUE);
	enum radio_sync_rx_mode rx_mode = RADIO_SYNC_RX_ACQUIRE;
	uint32_t consecutive_missed_windows = 0;
	uint32_t locked_beacon_count = 0;
	bool rx_window_pending = false;
	bool pps_started = false;
	int ret;

	reset_slave_sync_state(&filter, &cfg, &runtime_cfg, &seq, &last_rx_tick,
			       &missed_accounted_tick, &next_expected_master_tx,
			       &rx_mode, &next_rx_window,
			       &expected_rx_deadline,
			       &consecutive_missed_windows, &locked_beacon_count,
			       &rx_window_pending);

	LOG_INF("role=slave network=0x%08x rf_channel=%u interval=%uus",
		runtime_cfg.network_id, runtime_cfg.rf_channel,
		runtime_cfg.sync_interval_us);

	ret = pps_output_start_periodic(free_run_pps,
					TIME_SYNC_PPS_PERIOD_US_VALUE);
	if (ret == 0) {
		pps_started = true;
	} else {
		LOG_WRN("free-run pps start failed: %d", ret);
	}

	while (true) {
		struct ble_time_sync_command cmd;
		struct radio_sync_rx rx;

		now = timebase_now_us();

		while (ble_time_sync_get_command(&cmd) == 0) {
			handle_slave_ble_command(
				&cmd, &filter, &cfg, &runtime_cfg, &seq,
				&last_rx_tick, &missed_accounted_tick,
				&next_expected_master_tx, &rx_mode,
				&next_rx_window, &expected_rx_deadline,
				&consecutive_missed_windows, &locked_beacon_count,
				&rx_window_pending);
		}

		if (!rx_window_pending &&
		    rx_mode != RADIO_SYNC_RX_STOPPED &&
		    (int64_t)(now + slave_rx_request_ahead_us(
				      runtime_cfg.sync_interval_us) -
			      next_rx_window) >= 0) {
			uint32_t window_len = slave_rx_window_length_us(rx_mode);
			uint64_t window_start = next_rx_window;

			if (window_start <= now + SLAVE_RX_MIN_START_AHEAD_US) {
				window_start = now + SLAVE_RX_MIN_START_AHEAD_US;
			}
			struct radio_sync_window window = {
				.start_tick = window_start,
				.length_us = window_len,
				.mode = rx_mode,
			};

			ret = radio_sync_schedule_rx_window(&window);
			if (ret == 0) {
				rx_window_pending = true;
				expected_rx_deadline = window.start_tick + window_len;
			} else if (ret == -EAGAIN) {
				next_rx_window = window_start;
			} else if (ret == -ETIME) {
				rx_window_pending = false;
				slave_note_missed_rx_window(
					&filter, &cfg, &runtime_cfg, &rx_mode,
					&next_rx_window, &next_expected_master_tx,
					&last_rx_tick, &missed_accounted_tick,
					&consecutive_missed_windows,
					&locked_beacon_count);
			} else {
				LOG_WRN("radio rx window failed: %d", ret);
				next_rx_window = now + slave_rx_retry_gap_us();
			}
		}

		const struct radio_sync_stats *radio_stats = radio_sync_stats_get();
		bool rx_window_missed = rx_window_pending &&
			!radio_stats->rx_request_active;

		if (radio_sync_poll_rx(&rx) == 0) {
			int filter_ret;
			struct sync_observation obs = {
				.master_tick = rx.beacon.master_tx_tick +
					       TIME_SYNC_RADIO_DELAY_US_VALUE,
				.local_tick = rx.local_rx_tick,
				.next_pps_master_tick =
					rx.beacon.next_pps_master_tick,
			};
			uint64_t local_pps;
			filter_ret = sync_filter_update(&filter, &obs);
			if (filter_ret != 0 &&
			    sync_filter_state(&filter) != SYNC_FILTER_UNLOCKED &&
			    sync_filter_state(&filter) != SYNC_FILTER_ACQUIRING) {
				LOG_WRN("sync outlier, reacquiring: %d", filter_ret);
				cfg.sync_interval_us = runtime_cfg.sync_interval_us;
				sync_filter_init(&filter, &cfg);
				filter_ret = sync_filter_update(&filter, &obs);
			}

			last_rx_tick = rx.local_rx_tick;
			missed_accounted_tick = rx.local_rx_tick;
			seq = rx.beacon.seq;
			rx_window_pending = false;
			consecutive_missed_windows = 0;

			if (filter_ret == 0 &&
			    sync_filter_master_to_local(
				    &filter, rx.beacon.next_pps_master_tick,
				    &local_pps) == 0) {
				local_pps = time_sync_select_pps_target(
					local_pps,
					TIME_SYNC_PPS_PERIOD_US_VALUE,
					timebase_now_us(),
					SLAVE_PPS_KEEP_PENDING_WINDOW_US,
					pps_output_scheduled_tick(),
					SLAVE_PPS_MIN_ARM_AHEAD_US);

				if (!pps_started) {
					ret = pps_output_start_periodic(
						local_pps,
						TIME_SYNC_PPS_PERIOD_US_VALUE);
					if (ret == 0) {
						pps_started = true;
					}
				} else {
					ret = pps_output_reset_epoch(local_pps);
				}
				if (ret != 0 && ret != -ETIME) {
					LOG_WRN("pps schedule failed: %d", ret);
				}
			}

			if (filter_ret == 0) {
				uint64_t next_master_tx =
					rx.beacon.master_tx_tick +
					runtime_cfg.sync_interval_us;
				enum radio_sync_rx_mode planned_mode =
					RADIO_SYNC_RX_RECOVERY_WINDOW;

				next_expected_master_tx = next_master_tx;
				if (sync_filter_state(&filter) == SYNC_FILTER_LOCKED) {
					planned_mode = RADIO_SYNC_RX_LOCKED_WINDOW;
					if (locked_beacon_count <
					    SLAVE_BLE_START_LOCKED_BEACONS) {
						locked_beacon_count++;
					}
				} else {
					locked_beacon_count = 0;
				}

				if (slave_plan_predicted_window(
					    &filter, next_master_tx,
					    planned_mode,
					    &next_rx_window) == 0 &&
				    next_rx_window > now + SLAVE_RX_MIN_START_AHEAD_US) {
					rx_mode = planned_mode;
				} else {
					rx_mode = RADIO_SYNC_RX_ACQUIRE;
					next_rx_window = now + slave_rx_retry_gap_us();
					next_expected_master_tx = 0;
				}
			} else {
				locked_beacon_count = 0;
				rx_mode = RADIO_SYNC_RX_ACQUIRE;
				next_rx_window = now + slave_rx_retry_gap_us();
				next_expected_master_tx = 0;
			}

			if (!ble_time_sync_started() &&
			    locked_beacon_count >= SLAVE_BLE_START_LOCKED_BEACONS) {
				ret = ble_time_sync_start();
				if (ret == 0) {
					LOG_INF("BLE start after sync locked seq=%u",
						seq);
				} else {
					locked_beacon_count = 0;
					LOG_WRN("BLE start failed: %d", ret);
				}
			}
		} else if (rx_window_missed) {
			rx_window_pending = false;
			slave_note_missed_rx_window(
				&filter, &cfg, &runtime_cfg, &rx_mode,
				&next_rx_window, &next_expected_master_tx,
				&last_rx_tick, &missed_accounted_tick,
				&consecutive_missed_windows,
				&locked_beacon_count);
		}

		if (last_rx_tick != 0 &&
		    now - missed_accounted_tick > runtime_cfg.sync_interval_us) {
			uint32_t missed =
				(uint32_t)((now - missed_accounted_tick) /
					   runtime_cfg.sync_interval_us);

			sync_filter_note_missed(&filter, missed);
			missed_accounted_tick +=
				(uint64_t)missed * runtime_cfg.sync_interval_us;
		}

		if ((int64_t)(now - next_status) >= 0) {
			struct status_snapshot snapshot = {
				.role = "slave",
				.seq = seq,
				.filter_state = sync_filter_state(&filter),
				.offset_us = sync_filter_offset_us(&filter),
				.drift_ppm = sync_filter_drift_ppm(&filter),
				.missed_beacons = filter.missed_count,
				.next_pps_tick = filter.next_pps_master_tick,
				.last_beacon_age_us =
					last_rx_tick == 0 ? 0 : now - last_rx_tick,
				.ble_gate_count = locked_beacon_count,
				.ble_connected = ble_time_sync_connected(),
			};

			ble_time_sync_get_snapshot(&snapshot.ble);
			sync_filter_age(&filter,
					TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000u);
			if (sync_filter_state(&filter) != SYNC_FILTER_LOCKED &&
			    !ble_time_sync_started()) {
				locked_beacon_count = 0;
			}
			status_log(&snapshot);
			next_status = now + TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000ULL;
		}

		k_sleep(K_MSEC(SLAVE_SLEEP_MS));
	}
}
#endif

int main(void)
{
	int ret;

	ret = timebase_init();
	if (ret != 0) {
		LOG_ERR("timebase init failed: %d", ret);
		return 0;
	}

	ret = pps_output_init(TIME_SYNC_PPS_WIDTH_US_VALUE);
	if (ret != 0) {
		LOG_ERR("pps init failed: %d", ret);
		return 0;
	}

	runtime_config_init();
	struct time_sync_runtime_config runtime_cfg = runtime_config_get();

	ret = radio_sync_init(runtime_cfg.rf_channel, runtime_cfg.network_id);
	if (ret != 0) {
		LOG_ERR("radio init failed: %d", ret);
		return 0;
	}

	ret = ble_time_sync_init();
	if (ret != 0) {
		LOG_WRN("BLE init failed: %d", ret);
	}

#if defined(CONFIG_TIME_SYNC_LED_HEARTBEAT)
	ret = heartbeat_led_start(TIME_SYNC_LED_HEARTBEAT_PERIOD_MS_VALUE);
	if (ret != 0) {
		LOG_WRN("heartbeat LED disabled: %d", ret);
	}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
	master_loop();
#elif defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
	slave_loop();
#endif

	return 0;
}
