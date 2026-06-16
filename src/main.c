#include "app_config.h"
#include "heartbeat_led.h"
#include "pps_output.h"
#include "radio_sync.h"
#include "status.h"
#include "sync_filter.h"
#include "sync_packet.h"
#include "time_sync_math.h"
#include "timebase.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(time_sync_app, LOG_LEVEL_INF);

#define STATUS_SLEEP_MS 10
#define MASTER_BEACON_DEFAULT_ARM_AHEAD_US 80000u
#define MASTER_BEACON_TIMESLOT_GUARD_US 3000u
#define MASTER_BEACON_INTERVAL_GUARD_US 5000u
#define MASTER_BEACON_LATE_US 250u
#define SLAVE_PPS_KEEP_PENDING_WINDOW_US 20000u
#define SLAVE_PPS_MIN_ARM_AHEAD_US 1000u

#if defined(CONFIG_TIME_SYNC_ROLE_MASTER)
static uint32_t master_beacon_arm_ahead_us(void)
{
	uint32_t max_arm_ahead = TIME_SYNC_TIMESLOT_LENGTH_US_VALUE;
	uint32_t timeslot_overhead = TIME_SYNC_TIMESLOT_END_MARGIN_US_VALUE +
				     MASTER_BEACON_TIMESLOT_GUARD_US;

	if (max_arm_ahead > timeslot_overhead) {
		max_arm_ahead -= timeslot_overhead;
	} else {
		max_arm_ahead = MASTER_BEACON_TIMESLOT_GUARD_US;
	}

	if (TIME_SYNC_INTERVAL_US_VALUE > MASTER_BEACON_INTERVAL_GUARD_US) {
		uint32_t interval_arm_ahead =
			TIME_SYNC_INTERVAL_US_VALUE -
			MASTER_BEACON_INTERVAL_GUARD_US;

		if (max_arm_ahead > interval_arm_ahead) {
			max_arm_ahead = interval_arm_ahead;
		}
	} else {
		max_arm_ahead = TIME_SYNC_INTERVAL_US_VALUE / 2u;
	}

	return max_arm_ahead < MASTER_BEACON_DEFAULT_ARM_AHEAD_US ?
		max_arm_ahead : MASTER_BEACON_DEFAULT_ARM_AHEAD_US;
}

static void master_loop(void)
{
	uint16_t seq = 0;
	uint64_t now = timebase_now_us();
	uint32_t arm_ahead_us = master_beacon_arm_ahead_us();
	uint64_t next_beacon = now + arm_ahead_us;
	uint64_t next_pps = time_sync_next_period_tick(next_beacon,
						       TIME_SYNC_PPS_PERIOD_US_VALUE);
	uint64_t next_status = next_beacon;
	int ret;

	LOG_INF("role=master network=0x%08x rf_channel=%u interval=%uus "
		"txen_to_address=%uus tx_arm_ahead=%uus",
		TIME_SYNC_NETWORK_ID_VALUE, TIME_SYNC_RF_CHANNEL_VALUE,
		TIME_SYNC_INTERVAL_US_VALUE,
		TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US_VALUE,
		arm_ahead_us);

	ret = pps_output_start_periodic(next_pps,
					TIME_SYNC_PPS_PERIOD_US_VALUE);
	if (ret != 0) {
		LOG_ERR("master pps start failed: %d", ret);
		return;
	}

	while (true) {
		now = timebase_now_us();

		if (next_beacon <= now + MASTER_BEACON_LATE_US) {
			uint64_t missed =
				((now + MASTER_BEACON_LATE_US - next_beacon) /
				 TIME_SYNC_INTERVAL_US_VALUE) + 1u;

			next_beacon += missed * TIME_SYNC_INTERVAL_US_VALUE;
		}

		if ((int64_t)(now + arm_ahead_us - next_beacon) >= 0) {
			uint64_t master_tx_tick = next_beacon +
				TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US_VALUE;
			uint64_t pps_tick = time_sync_next_epoch_tick_after(
				pps_output_stats_get()->epoch_tick,
				TIME_SYNC_PPS_PERIOD_US_VALUE,
				master_tx_tick);
			struct sync_beacon beacon = {
				.magic = SYNC_BEACON_MAGIC,
				.version = SYNC_BEACON_VERSION,
				.role = SYNC_BEACON_ROLE_MASTER,
				.seq = seq,
				.network_id = TIME_SYNC_NETWORK_ID_VALUE,
				.master_tx_tick = master_tx_tick,
				.next_pps_master_tick = pps_tick,
				.sync_interval_us = TIME_SYNC_INTERVAL_US_VALUE,
				.status_flags = 0,
			};
			ret = radio_sync_send_beacon_at(
				&beacon, next_beacon,
				TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US_VALUE,
				NULL);

			if (ret == 0) {
				seq++;
			} else {
				LOG_WRN("radio tx failed: %d", ret);
			}

			next_beacon += TIME_SYNC_INTERVAL_US_VALUE;
		}

		if ((int64_t)(now - next_status) >= 0) {
			struct status_snapshot snapshot = {
				.role = "master",
				.seq = seq,
				.filter_state = SYNC_FILTER_LOCKED,
				.offset_us = 0,
				.drift_ppm = 0,
				.missed_beacons = 0,
				.next_pps_tick =
					pps_output_stats_get()->scheduled_rise_tick,
				.last_beacon_age_us = 0,
			};

			status_log(&snapshot);
			next_status += TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000ULL;
		}

		k_sleep(K_MSEC(STATUS_SLEEP_MS));
	}
}
#endif

#if defined(CONFIG_TIME_SYNC_ROLE_SLAVE)
static void slave_loop(void)
{
	struct sync_filter filter;
	struct sync_filter_config cfg = sync_filter_default_config();
	uint16_t seq = 0;
	uint64_t last_rx_tick = 0;
	uint64_t missed_accounted_tick = 0;
	uint64_t now = timebase_now_us();
	uint64_t next_status = now;
	uint64_t free_run_pps = time_sync_next_period_tick(
		now + 10000u, TIME_SYNC_PPS_PERIOD_US_VALUE);
	bool pps_started = false;
	int ret;

	cfg.sync_interval_us = TIME_SYNC_INTERVAL_US_VALUE;
	sync_filter_init(&filter, &cfg);
	(void)radio_sync_start_rx();

	LOG_INF("role=slave network=0x%08x rf_channel=%u interval=%uus",
		TIME_SYNC_NETWORK_ID_VALUE, TIME_SYNC_RF_CHANNEL_VALUE,
		TIME_SYNC_INTERVAL_US_VALUE);

	ret = pps_output_start_periodic(free_run_pps,
					TIME_SYNC_PPS_PERIOD_US_VALUE);
	if (ret == 0) {
		pps_started = true;
	} else {
		LOG_WRN("free-run pps start failed: %d", ret);
	}

	while (true) {
		struct radio_sync_rx rx;

		now = timebase_now_us();

		if (radio_sync_poll_rx(&rx) == 0) {
			struct sync_observation obs = {
				.master_tick = rx.beacon.master_tx_tick +
					       TIME_SYNC_RADIO_DELAY_US_VALUE,
				.local_tick = rx.local_rx_tick,
				.next_pps_master_tick =
					rx.beacon.next_pps_master_tick,
			};
			uint64_t local_pps;
			ret = sync_filter_update(&filter, &obs);

			last_rx_tick = rx.local_rx_tick;
			missed_accounted_tick = rx.local_rx_tick;
			seq = rx.beacon.seq;

			if (ret == 0 &&
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

		}

		if (last_rx_tick != 0 &&
		    now - missed_accounted_tick > TIME_SYNC_INTERVAL_US_VALUE) {
			uint32_t missed =
				(uint32_t)((now - missed_accounted_tick) /
					   TIME_SYNC_INTERVAL_US_VALUE);

			sync_filter_note_missed(&filter, missed);
			missed_accounted_tick +=
				(uint64_t)missed * TIME_SYNC_INTERVAL_US_VALUE;
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
			};

			sync_filter_age(&filter,
					TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000u);
			status_log(&snapshot);
			next_status += TIME_SYNC_STATUS_INTERVAL_MS_VALUE * 1000ULL;
		}

		k_sleep(K_MSEC(STATUS_SLEEP_MS));
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

	ret = radio_sync_init(TIME_SYNC_RF_CHANNEL_VALUE,
			      TIME_SYNC_NETWORK_ID_VALUE);
	if (ret != 0) {
		LOG_ERR("radio init failed: %d", ret);
		return 0;
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
