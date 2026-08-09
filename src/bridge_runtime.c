#include "bridge_runtime.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "bridge_config.h"
#include "link_scheduler_core.h"
#include "nmea_parser.h"
#include "param_config.h"
#include "pps_input.h"
#include "radio_address_crypto.h"
#include "radio_transport.h"
#include "pps_output.h"
#include "sync_filter.h"
#include "time_sync_math.h"
#include "timebase.h"
#include "time_uart.h"
#include "uart_bridge.h"
#include "utc_clock.h"
#include "wireless_time_sync.h"

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
#include "bridge_validation.h"
#endif

LOG_MODULE_REGISTER(bridge_runtime, LOG_LEVEL_INF);

#ifndef CONFIG_RADIO_BRIDGE_THREAD_STACK_SIZE
#define CONFIG_RADIO_BRIDGE_THREAD_STACK_SIZE 4096
#endif

#ifndef CONFIG_RADIO_BRIDGE_MAX_SLAVES
#define CONFIG_RADIO_BRIDGE_MAX_SLAVES 3
#endif

#ifndef CONFIG_RADIO_BRIDGE_UART_RING_SIZE
#define CONFIG_RADIO_BRIDGE_UART_RING_SIZE 16384
#endif

K_THREAD_STACK_DEFINE(bridge_thread_stack, CONFIG_RADIO_BRIDGE_THREAD_STACK_SIZE);
K_MSGQ_DEFINE(bridge_wake_msgq, sizeof(uint8_t), 8, 4);

static struct k_thread bridge_thread;
static struct rb_scheduler_core scheduler;
static struct rb_bridge_stats bridge_stats;
static struct rb_radio_addresses radio_addresses;
static uint8_t group_key[RB_GROUP_KEY_BYTES];
static uint32_t runtime_group_id;
static uint32_t runtime_pps_period_us;
static uint32_t runtime_time_source_mode;
static uint32_t runtime_pps_input_delay_us;
static uint64_t local_device_id;
static bool initialized;
static bool started;
static enum rb_radio_profile current_profile;
static struct rb_wireless_time_sync wireless_sync;
static struct sync_filter sync_filter_runtime;
static uint8_t local_discovery_slot;
static uint32_t pending_sync_sequence;
static bool pending_sync_capture;
static struct rb_utc_clock utc_clock;
static uint64_t pending_external_pps_tick;
static int64_t pending_nmea_utc_seconds;
static uint64_t last_external_pps_tick;
static uint64_t last_nmea_tick;
static uint64_t sync_filter_tick;
static uint64_t next_expected_sync_tick;
static bool pending_external_pps;
static bool pending_nmea_utc;

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
static struct rb_validation_pipe validation_pipe;
static struct k_spinlock validation_lock;
static int64_t validation_pair_seconds;
static uint8_t validation_time_command;
#endif

static uint64_t read_device_id(void)
{
	uint8_t raw[16] = {0};
	ssize_t len = hwinfo_get_device_id(raw, sizeof(raw));
	if (len < 8) {
		return 0;
	}
	return sys_get_le64(raw);
}

static uint32_t new_nonzero_session(void)
{
	uint32_t session;
	do {
		session = sys_rand32_get();
	} while (session == 0u);
	return session;
}

/* ISR-safe: called from the ESB event handler and the UART async callback.
 * K_NO_WAIT never blocks; a full queue just means the thread is already
 * about to wake, so the dropped put is harmless. */
static void bridge_thread_wake(void)
{
	uint8_t wake = 0u;

	(void)k_msgq_put(&bridge_wake_msgq, &wake, K_NO_WAIT);
}

static int runtime_make_event_view(const struct rb_radio_event *event,
					   struct rb_radio_event_view *view)
{
	if (event == NULL || view == NULL) {
		return -EINVAL;
	}
	memset(view, 0, sizeof(*view));
	view->pipe = event->pipe;
	view->attempts = event->attempts;
	view->address_tick = event->address_tick;
	view->wire = event->data;
	view->wire_len = event->length;
	switch (event->type) {
	case RB_RADIO_EVENT_RX_RECEIVED:
		view->type = RB_EVENT_VIEW_RX_RECEIVED;
		return 0;
	case RB_RADIO_EVENT_TX_SUCCESS:
		view->type = RB_EVENT_VIEW_TX_SUCCESS;
		return 0;
	case RB_RADIO_EVENT_TX_FAILED:
		view->type = RB_EVENT_VIEW_TX_FAILED;
		return 0;
	default:
		return -EINVAL;
	}
}

static void runtime_note_utc_transition(enum rb_utc_state previous)
{
	enum rb_utc_state current = rb_utc_clock_state(&utc_clock);

	if (previous != RB_UTC_HOLDOVER && current == RB_UTC_HOLDOVER) {
		bridge_stats.holdover_count++;
	}
	bridge_stats.utc_state = (uint8_t)current;
}

static int runtime_phase_reset(void *context, uint64_t target_tick)
{
	uint64_t now_tick = timebase_now_us();

	ARG_UNUSED(context);
	if (now_tick > UINT64_MAX - 100u || target_tick < now_tick + 100u) {
		return -ETIME;
	}
	return pps_output_reset_epoch(target_tick);
}

static void runtime_try_external_pair(void)
{
	enum rb_utc_state previous;
	uint64_t now_tick;
	int ret;

	if (!pending_external_pps || !pending_nmea_utc) {
		return;
	}
	now_tick = timebase_now_us();
	if (now_tick < pending_external_pps_tick) {
		now_tick = pending_external_pps_tick;
	}
	previous = rb_utc_clock_state(&utc_clock);
	rb_utc_clock_tick(&utc_clock, now_tick);
	ret = rb_utc_clock_note_pair(&utc_clock, pending_external_pps_tick,
				     pending_nmea_utc_seconds);
	if (ret != 0) {
		bridge_stats.nmea_drop_count++;
	}
	pending_external_pps = false;
	pending_nmea_utc = false;
	runtime_note_utc_transition(previous);
}

static void runtime_consume_external_time(void)
{
	struct rb_nmea_utc utc;
	char line[RB_NMEA_MAX_SENTENCE_LENGTH + 1u];
	uint64_t capture_tick;
	size_t line_len;

	if (!scheduler.config.master || runtime_time_source_mode != 1u) {
		return;
	}
	while (pps_input_poll(&capture_tick, K_NO_WAIT) == 0) {
		bridge_stats.external_pps_count++;
		last_external_pps_tick = timebase_now_us();
		if (capture_tick < runtime_pps_input_delay_us) {
			continue;
		}
		pending_external_pps_tick =
			capture_tick - runtime_pps_input_delay_us;
		pending_external_pps = true;
	}
	while ((line_len = time_uart_read_line(line, sizeof(line))) != 0u) {
		if (rb_nmea_parse_sentence(line, line_len, &utc) != 0 ||
		    rb_nmea_utc_to_unix(&utc, &pending_nmea_utc_seconds) != 0) {
			bridge_stats.nmea_drop_count++;
			continue;
		}
		bridge_stats.nmea_valid_count++;
		last_nmea_tick = timebase_now_us();
		pending_nmea_utc = true;
	}
	runtime_try_external_pair();
}

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
static void runtime_consume_validation_time(void)
{
	int64_t pair_seconds;
	uint8_t command;
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	command = validation_time_command;
	pair_seconds = validation_pair_seconds;
	validation_time_command = 0u;
	k_spin_unlock(&validation_lock, key);
	if (command == 1u) {
		uint64_t pair_tick = utc_clock.have_pair ?
			utc_clock.last_pair_tick + 1000000u : timebase_now_us();

		bridge_stats.external_pps_count++;
		bridge_stats.nmea_valid_count++;
		last_external_pps_tick = pair_tick;
		last_nmea_tick = pair_tick;
		pending_external_pps_tick = pair_tick;
		pending_nmea_utc_seconds = pair_seconds;
		pending_external_pps = true;
		pending_nmea_utc = true;
		runtime_try_external_pair();
	} else if (command == 2u) {
		enum rb_utc_state previous = rb_utc_clock_state(&utc_clock);
		uint64_t age_tick = utc_clock.have_pair ?
			utc_clock.last_pair_tick + 3000000u :
			timebase_now_us() + 3000000u;

		last_external_pps_tick = age_tick - 3000000u;
		last_nmea_tick = age_tick - 3000000u;
		rb_utc_clock_tick(&utc_clock, age_tick);
		runtime_note_utc_transition(previous);
	}
}
#endif

static void runtime_apply_wireless_utc(const struct rb_sync_discovery *frame)
{
	if (frame->time_quality == RB_TIME_HOLDOVER &&
	    bridge_stats.utc_quality != RB_TIME_HOLDOVER) {
		bridge_stats.holdover_count++;
	}
	bridge_stats.utc_quality = frame->time_quality;
	bridge_stats.utc_seconds = frame->time_quality == RB_TIME_UTC_INVALID ?
		0 : frame->next_pps_utc_seconds;
}

static void runtime_time_tick(uint64_t now_tick)
{
	uint64_t elapsed = sync_filter_tick == 0u || now_tick < sync_filter_tick ?
		0u : now_tick - sync_filter_tick;
	uint32_t missed = 0u;
	struct rb_utc_publication publication;

	if (!scheduler.config.master && next_expected_sync_tick != 0u &&
	    now_tick >= next_expected_sync_tick) {
		uint64_t interval = scheduler.config.sync_interval_us;
		uint64_t count = (now_tick - next_expected_sync_tick) / interval + 1u;

		missed = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
		next_expected_sync_tick += count * interval;
		bridge_stats.sync_missed_count += count;
	}
	sync_filter_note_missed(&sync_filter_runtime, missed);
	sync_filter_age(&sync_filter_runtime,
		elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
	sync_filter_tick = now_tick;
	if (!scheduler.config.master) {
		return;
	}
	{
		enum rb_utc_state previous = rb_utc_clock_state(&utc_clock);
		uint64_t publication_tick = now_tick < utc_clock.now_tick ?
			utc_clock.now_tick : now_tick;

		rb_utc_clock_tick(&utc_clock, publication_tick);
		runtime_note_utc_transition(previous);
		if (rb_utc_clock_publication(&utc_clock, publication_tick,
					     &publication) != 0) {
			publication = (struct rb_utc_publication){0};
		}
	}
	if (rb_scheduler_set_time_publication(&scheduler,
		publication.valid ? publication.next_pps_utc_seconds : 0,
		(uint8_t)publication.quality) == 0) {
		bridge_stats.utc_quality = (uint8_t)publication.quality;
		bridge_stats.utc_seconds = publication.valid ?
			publication.next_pps_utc_seconds : 0;
	}
}

static void runtime_apply_sync(const struct rb_radio_event *event)
{
	struct rb_sync_discovery frame;
	struct sync_observation observation;
	uint64_t local_pps;
	int ret;

	if (scheduler.config.master || event == NULL ||
	    event->type != RB_RADIO_EVENT_RX_RECEIVED ||
	    rb_sync_discovery_decode(event->data, event->length, &frame) != 0) {
		return;
	}
	if (frame.group_id != runtime_group_id) {
		return;
	}
	if (frame.common.master_session == 0u || frame.sync_sequence == 0u ||
	    frame.sync_interval_us == 0u) {
		return;
	}
	if (wireless_sync.session != frame.common.master_session) {
		sync_filter_reset(&sync_filter_runtime);
		bridge_stats.sync_offset = 0;
		bridge_stats.sync_jitter = 0u;
		bridge_stats.sync_last_error = 0;
		bridge_stats.sync_relock_count++;
	}
	if (rb_discovery_slot_nrf(group_key, frame.discovery_nonce,
					 local_device_id, &local_discovery_slot) == 0) {
		rb_scheduler_set_discovery_slot(&scheduler, local_discovery_slot);
	}
	bridge_stats.sync_rx_count++;
	bridge_stats.sync_last_sequence = frame.sync_sequence;
	bridge_stats.sync_last_previous_master_tick =
		frame.previous_master_address_tick;
	ret = wireless_time_sync_slave_receive(&wireless_sync, &frame,
						 event->address_tick, &observation);
	bridge_stats.sync_last_error = ret;
	if ((ret == 0 || ret == -EAGAIN) &&
	    wireless_sync.session == frame.common.master_session) {
		runtime_apply_wireless_utc(&frame);
		next_expected_sync_tick = event->address_tick + frame.sync_interval_us;
	}
	if (ret != 0) {
		if (ret == -EAGAIN) {
			bridge_stats.sync_tracker_wait_count++;
		} else {
			bridge_stats.sync_tracker_error_count++;
			bridge_stats.sync_missed_count++;
		}
		return;
	}
	bridge_stats.sync_pair_count++;
	ret = sync_filter_update(&sync_filter_runtime, &observation);
	bridge_stats.sync_last_error = ret;
	if (ret != 0) {
		bridge_stats.sync_filter_error_count++;
		return;
	}
	bridge_stats.sync_filter_update_count++;
	if (sync_filter_master_to_local(&sync_filter_runtime,
					observation.next_pps_master_tick,
					&local_pps) != 0) {
		return;
	}
	local_pps = time_sync_select_pps_target(local_pps,
						       runtime_pps_period_us,
						       timebase_now_us(),
						       runtime_pps_period_us,
						       pps_output_scheduled_tick(), 100u);
	ret = pps_output_reset_epoch(local_pps);
	if (ret != 0) {
		bridge_stats.sync_pps_reset_error_count++;
		bridge_stats.sync_last_pps_reset_error = ret;
		bridge_stats.sync_missed_count++;
	}
	{
		int64_t new_offset = sync_filter_offset_us(&sync_filter_runtime);
		int64_t diff = new_offset - bridge_stats.sync_offset;
		bridge_stats.sync_jitter = (uint32_t)(diff < 0 ? -diff : diff);
		bridge_stats.sync_offset = new_offset;
	}
}

static void runtime_pull_uart_rx(void)
{
	uint8_t data[256];
	size_t len;
	do {
		len = uart_bridge_read(data, sizeof(data));
		if (len != 0u) {
			(void)rb_scheduler_uart_write(&scheduler, data, len,
						      timebase_now_us());
		}
	} while (len == sizeof(data));
}

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
static void runtime_pull_validation_rx(void)
{
	uint8_t data[256];

	for (;;) {
		size_t available = rb_scheduler_uart_available(&scheduler);
		size_t max_len = available < sizeof(data) ? available : sizeof(data);
		size_t len;

		if (max_len == 0u) {
			return;
		}
		k_spinlock_key_t key = k_spin_lock(&validation_lock);

		len = rb_validation_pull_input(&validation_pipe, data, max_len);
		k_spin_unlock(&validation_lock, key);
		if (len == 0u) {
			return;
		}
		(void)rb_scheduler_uart_write(&scheduler, data, len,
					      timebase_now_us());
	}
}
#endif

static void runtime_push_uart_tx(void)
{
	uint8_t data[256];
	size_t len;
	do {
		if (scheduler.config.master) {
			len = rb_scheduler_master_read_uart(&scheduler, data,
								 sizeof(data));
		} else {
			len = rb_scheduler_slave_read_uart(&scheduler, data,
								 sizeof(data));
		}
		if (len != 0u) {
		#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
			k_spinlock_key_t key = k_spin_lock(&validation_lock);

			(void)rb_validation_capture_output(&validation_pipe, data, len);
			k_spin_unlock(&validation_lock, key);
		#endif
			(void)uart_bridge_write(data, len);
		}
	} while (len == sizeof(data));
}

static int runtime_execute_action(struct rb_scheduler_action *action)
{
	uint8_t temporary[5];
	int ret;

	if (action == NULL) {
		return -EINVAL;
	}
	switch (action->type) {
	case RB_ACTION_SEND_HELLO:
		/* HELLO is sent on the stable group address.  The candidate switches to
		 * its temporary DEVICEID-derived address only while awaiting ASSIGN. */
		ret = radio_transport_set_profile(RB_RADIO_SLAVE_HELLO_PTX, 0, NULL);
		if (ret != 0) {
			return ret;
		}
		bridge_stats.discovery_hello_count++;
		current_profile = RB_RADIO_SLAVE_HELLO_PTX;
		return radio_transport_send(0, true, action->wire, action->wire_len);
	case RB_ACTION_ENTER_ASSIGN_RX:
		ret = rb_temporary_address_derive_nrf(runtime_group_id,
						 group_key, local_device_id, temporary);
		if (ret != 0) {
			return ret;
		}
		ret = radio_transport_set_profile(RB_RADIO_SLAVE_ASSIGN_PRX, 0,
						 temporary);
		if (ret == 0) {
			current_profile = RB_RADIO_SLAVE_ASSIGN_PRX;
		}
		return ret;
	case RB_ACTION_ENTER_GROUP_RX:
		ret = radio_transport_set_profile(RB_RADIO_SLAVE_GROUP_PRX, 0, NULL);
		if (ret == 0) {
			current_profile = RB_RADIO_SLAVE_GROUP_PRX;
		}
		return ret;
	case RB_ACTION_ENTER_DISCOVERY_RX:
		ret = radio_transport_set_profile(RB_RADIO_MASTER_DISCOVERY_PRX, 0, NULL);
		if (ret == 0) {
			current_profile = RB_RADIO_MASTER_DISCOVERY_PRX;
		}
		return ret;
	case RB_ACTION_QUEUE_ACK:
		return radio_transport_queue_ack(action->pipe, action->wire,
						 action->wire_len);
	case RB_ACTION_SEND_SYNC_DISCOVERY:
	case RB_ACTION_SEND_DOWNLINK_BROADCAST:
	case RB_ACTION_SEND_REPAIR:
	case RB_ACTION_SEND_SKIP_TO:
	case RB_ACTION_SEND_POLL:
		if (scheduler.config.master &&
		    current_profile != RB_RADIO_MASTER_PTX) {
			ret = radio_transport_set_profile(RB_RADIO_MASTER_PTX, 0, NULL);
			if (ret != 0) {
				return ret;
			}
			current_profile = RB_RADIO_MASTER_PTX;
		}
		if (action->type == RB_ACTION_SEND_SYNC_DISCOVERY) {
			struct rb_sync_discovery scheduled;
			struct rb_sync_discovery published;
			uint64_t next_pps = pps_output_scheduled_tick();
			if (rb_sync_discovery_decode(action->wire, action->wire_len,
							     &scheduled) != 0) {
				return -EBADMSG;
			}
			if (next_pps <= timebase_now_us()) {
				next_pps = timebase_now_us() +
					runtime_pps_period_us;
			}
			if (wireless_time_sync_master_build(&wireless_sync, next_pps,
							      &published) != 0) {
				return -EINVAL;
			}
			published.common = scheduled.common;
			published.group_id = scheduled.group_id;
			published.master_id = scheduled.master_id;
			published.sync_sequence = scheduled.sync_sequence;
			published.sync_interval_us = scheduled.sync_interval_us;
			published.discovery_nonce = scheduled.discovery_nonce;
			published.free_slots = scheduled.free_slots;
			published.response_slot_count = scheduled.response_slot_count;
			published.response_slot_us = scheduled.response_slot_us;
			published.next_pps_utc_seconds =
				scheduled.next_pps_utc_seconds;
			published.time_quality = scheduled.time_quality;
			if (rb_sync_discovery_encode(&published, action->wire,
						     sizeof(action->wire),
						     &action->wire_len) != 0) {
				return -EINVAL;
			}
			bridge_stats.sync_tx_build_count++;
			bridge_stats.sync_last_tx_sequence = published.sync_sequence;
			if (published.previous_master_address_tick != 0u) {
				bridge_stats.sync_tx_previous_count++;
			}
			pending_sync_sequence = published.sync_sequence;
			pending_sync_capture = true;
		}
		ret = radio_transport_send(action->pipe, action->no_ack,
						 action->wire, action->wire_len);
		if (ret == 0) {
			bridge_stats.radio_tx_packets++;
			if (action->type == RB_ACTION_SEND_DOWNLINK_BROADCAST) {
				bridge_stats.broadcast_packets++;
			} else if (action->type == RB_ACTION_SEND_POLL) {
				bridge_stats.poll_packets++;
			} else if (action->type == RB_ACTION_SEND_REPAIR) {
				bridge_stats.repair_packets++;
			}
		}
		return ret;
	case RB_ACTION_SEND_ASSIGN:
		ret = rb_temporary_address_derive_nrf(runtime_group_id,
						 group_key, action->target_device_id,
						 temporary);
		if (ret != 0) {
			return ret;
		}
		ret = radio_transport_set_profile(RB_RADIO_MASTER_ASSIGN_PTX,
						 action->node_id, temporary);
		if (ret != 0) {
			return ret;
		}
		current_profile = RB_RADIO_MASTER_ASSIGN_PTX;
		/* The temporary DEVICEID address is installed as ESB pipe 0 on the
		 * candidate.  The scheduler pipe is the eventual node ID and is kept
		 * for membership bookkeeping; it must not select a stable group prefix
		 * while the temporary profile is active. */
		ret = radio_transport_send(0, false,
						 action->wire, action->wire_len);
		if (ret == 0) {
			bridge_stats.radio_tx_packets++;
		}
		return ret;
	case RB_ACTION_NONE:
	default:
		return 0;
	}
}

static void bridge_thread_fn(void *p1, void *p2, void *p3)
{
	struct rb_radio_event event;
	struct rb_radio_event_view view;
	struct rb_scheduler_action action;
	enum rb_scheduler_action_type last_scheduler_action = RB_ACTION_NONE;
	int last_scheduler_ret = 0;
	bool scheduler_ret_recorded = false;
	uint8_t wake;
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		while (radio_transport_get_event(&event, K_NO_WAIT) == 0) {
			if (pending_sync_capture &&
			    event.type == RB_RADIO_EVENT_TX_SUCCESS) {
				wireless_time_sync_master_tx_captured(&wireless_sync,
							pending_sync_sequence,
							event.address_tick);
				bridge_stats.sync_tx_capture_count++;
				bridge_stats.sync_last_tx_sequence = pending_sync_sequence;
				bridge_stats.sync_last_tx_tick = event.address_tick;
				pending_sync_capture = false;
			} else if (pending_sync_capture &&
				   event.type == RB_RADIO_EVENT_TX_FAILED) {
				bridge_stats.sync_tx_failure_count++;
				pending_sync_capture = false;
			}
			if (event.type == RB_RADIO_EVENT_TX_SUCCESS) {
				if (event.attempts > 1u) {
					bridge_stats.radio_retry_count += event.attempts - 1u;
				}
			} else if (event.type == RB_RADIO_EVENT_TX_FAILED) {
				bridge_stats.radio_retry_exhausted++;
				bridge_stats.radio_retry_count += event.attempts;
			}
			if (event.type == RB_RADIO_EVENT_RX_RECEIVED) {
				bridge_stats.radio_rx_packets++;
			}
			runtime_apply_sync(&event);
			if (runtime_make_event_view(&event, &view) == 0) {
				rb_scheduler_on_radio_event(&scheduler, &view,
							    timebase_now_us());
			}
		}
		runtime_pull_uart_rx();
		runtime_consume_external_time();
	#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
		runtime_pull_validation_rx();
		runtime_consume_validation_time();
	#endif
		for (;;) {
			uint32_t evicted;
			int action_ret;
			int scheduler_ret;

			uint64_t now_tick = timebase_now_us();

			runtime_time_tick(now_tick);
			scheduler_ret = rb_scheduler_next_action(&scheduler,
							 now_tick, &action);
			if (!scheduler_ret_recorded || scheduler_ret != last_scheduler_ret ||
			    action.type != last_scheduler_action) {
				radio_transport_retained_diag_note(
					RB_RADIO_BOOT_STAGE_SCHEDULER_NEXT_RETURNED,
					(uint8_t)action.type, scheduler_ret);
				last_scheduler_ret = scheduler_ret;
				last_scheduler_action = action.type;
				scheduler_ret_recorded = true;
			}
			if (scheduler_ret != 0 || action.type == RB_ACTION_NONE) {
				break;
			}
			if (action.type == RB_ACTION_SEND_SKIP_TO) {
				bridge_stats.unrecoverable_gap_count++;
			}
			radio_transport_retained_diag_note(
				RB_RADIO_BOOT_STAGE_ACTION, (uint8_t)action.type, 0);
			bridge_stats.last_action = (uint8_t)action.type;
			action_ret = runtime_execute_action(&action);
			if (action_ret != 0) {
				bridge_stats.action_error_count++;
				bridge_stats.last_action_error = action_ret;
				bridge_stats.last_action_error_action =
					(uint8_t)action.type;
				rb_scheduler_action_failed(&scheduler, &action,
						   timebase_now_us());
			}
			radio_transport_retained_diag_note(
				RB_RADIO_BOOT_STAGE_ACTION_RETURNED,
				(uint8_t)action.type, action_ret);
			if (action_ret == -EBUSY) {
				break;
			}
			/* After executing the action, check if the TX history window
			 * evicted a slot to make room — that represents a packet that
			 * could not be repaired before the window wrapped. */
			while (rb_scheduler_take_evicted(&scheduler, &evicted)) {
				bridge_stats.radio_history_drop_packets++;
			}
		}
		runtime_push_uart_tx();
		/* radio_transport and uart_bridge both wake this queue as soon as a
		 * new event/byte arrives; the timeout is only a fallback so a
		 * missed wake (e.g. a scheduler deadline expiring with no new I/O)
		 * still gets serviced promptly. */
		(void)k_msgq_get(&bridge_wake_msgq, &wake, K_USEC(100));
	}
}

int bridge_runtime_init(void)
{
	struct rb_scheduler_config config;
	struct rb_radio_transport_config radio_config;
	uint32_t group_id, sync_interval_us, aggregation_timeout_us, time_uart_baud;
	uint32_t max_idle_poll_us, lease_timeout_us, assignment_window_us;
	uint32_t response_slot_count, response_slot_us;
	int ret;

	if (initialized) {
		return 0;
	}

	/* Read configurable parameters from param system */
	ret = rb_param_get_uint32(RB_PARAM_GROUP_ID, &group_id);
	if (ret != 0) {
		LOG_ERR("Failed to read group_id: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_SYNC_INTERVAL_US, &sync_interval_us);
	if (ret != 0) {
		LOG_ERR("Failed to read sync_interval_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_AGGREGATION_TIMEOUT_US, &aggregation_timeout_us);
	if (ret != 0) {
		LOG_ERR("Failed to read aggregation_timeout_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_IDLE_POLL_MAX_US, &max_idle_poll_us);
	if (ret != 0) {
		LOG_ERR("Failed to read max_idle_poll_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_LEASE_TIMEOUT_US, &lease_timeout_us);
	if (ret != 0) {
		LOG_ERR("Failed to read lease_timeout_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_ASSIGNMENT_WINDOW_US, &assignment_window_us);
	if (ret != 0) {
		LOG_ERR("Failed to read assignment_window_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_RESPONSE_SLOT_COUNT, &response_slot_count);
	if (ret != 0) {
		LOG_ERR("Failed to read response_slot_count: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_RESPONSE_SLOT_US, &response_slot_us);
	if (ret != 0) {
		LOG_ERR("Failed to read response_slot_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_PPS_PERIOD_US, &runtime_pps_period_us);
	if (ret != 0) {
		LOG_ERR("Failed to read pps_period_us: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_TIME_UART_BAUDRATE, &time_uart_baud);
	if (ret != 0) {
		LOG_ERR("Failed to read time_uart_baudrate: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_TIME_SOURCE_MODE,
				  &runtime_time_source_mode);
	if (ret != 0) {
		LOG_ERR("Failed to read time_source_mode: %d", ret);
		return ret;
	}
	ret = rb_param_get_uint32(RB_PARAM_PPS_INPUT_DELAY_US,
				  &runtime_pps_input_delay_us);
	if (ret != 0) {
		LOG_ERR("Failed to read pps_input_delay_us: %d", ret);
		return ret;
	}

	/* Read group_key from param system */
	size_t key_len = RB_GROUP_KEY_BYTES;
	ret = rb_param_get_bytes(RB_PARAM_GROUP_KEY, group_key, &key_len);
	if (ret != 0) {
		LOG_ERR("Failed to read group_key: %d", ret);
		return ret;
	}
	if (key_len != RB_GROUP_KEY_BYTES) {
		LOG_ERR("Invalid group_key length: %zu", key_len);
		return -EINVAL;
	}
	if (!rb_group_id_valid(group_id)) {
		return -EINVAL;
	}
	runtime_group_id = group_id;
	local_device_id = read_device_id();
	if (local_device_id == 0u) {
		return -ENODEV;
	}

	uint32_t role_id;
	ret = rb_param_get_uint32(RB_PARAM_ROLE_ID, &role_id);
	if (ret != 0) {
		LOG_ERR("Failed to read role_id: %d", ret);
		return ret;
	}

	bool is_master = (role_id == 0u);

	ret = rb_radio_addresses_derive_nrf(group_id, group_key, &radio_addresses);
	if (ret != 0) {
		return ret;
	}

	config = (struct rb_scheduler_config){
		.master = is_master,
		.group_id = group_id,
		.master_session = new_nonzero_session(),
		.device_id = local_device_id,
		.sync_interval_us = sync_interval_us,
		.aggregation_timeout_us = aggregation_timeout_us,
		.max_idle_poll_us = max_idle_poll_us,
		.lease_timeout_us = lease_timeout_us,
		.assignment_window_us = assignment_window_us,
		.response_slot_count = (uint8_t)response_slot_count,
		.response_slot_us = (uint16_t)response_slot_us,
	};

	LOG_INF("Bridge runtime: role=%u (%s), device_id=%016llx, group_id=%u",
		role_id,
		is_master ? "master" : "slave",
		(unsigned long long)local_device_id,
		group_id);

	rb_scheduler_init(&scheduler, &config);
	memset(&bridge_stats, 0, sizeof(bridge_stats));
	bridge_stats.is_master = config.master ? 1u : 0u;
	bridge_stats.time_source_mode = (uint8_t)runtime_time_source_mode;
	#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
	rb_validation_pipe_init(&validation_pipe);
	#endif
	{
		struct sync_filter_config filter_config = sync_filter_default_config();
		filter_config.sync_interval_us = sync_interval_us;
		sync_filter_init(&sync_filter_runtime, &filter_config);
	}
	wireless_time_sync_init(&wireless_sync, config.master_session,
					sync_interval_us, 0);
	{
		struct rb_utc_clock_config utc_config = {
			.external_mode = config.master && runtime_time_source_mode == 1u,
			.loss_timeout_us = 3000000u,
			.phase_reset = runtime_phase_reset,
		};

		rb_utc_clock_init(&utc_clock, &utc_config);
		bridge_stats.utc_state = (uint8_t)rb_utc_clock_state(&utc_clock);
		bridge_stats.utc_quality = RB_TIME_UTC_INVALID;
	}
	ret = uart_bridge_init();
	if (ret != 0) {
		return ret;
	}
	uart_bridge_set_wake_callback(bridge_thread_wake);
	radio_config = (struct rb_radio_transport_config){
		.master = config.master,
		.group_id = group_id,
		.addresses = radio_addresses,
	};
	ret = radio_transport_init(&radio_config);
	if (ret != 0) {
		return ret;
	}
	radio_transport_set_wake_callback(bridge_thread_wake);
	if (runtime_time_source_mode == 1u && config.master) {
		ret = pps_input_init();
		if (ret != 0) {
			return ret;
		}
		time_uart_set_wake_callback(bridge_thread_wake);
		ret = time_uart_init(time_uart_baud);
		if (ret != 0) {
			return ret;
		}
	}

	current_profile = config.master ? RB_RADIO_MASTER_PTX :
		RB_RADIO_SLAVE_GROUP_PRX;
	initialized = true;
	return 0;
}

int bridge_runtime_start(void)
{
	if (!initialized) {
		return -EACCES;
	}
	if (started) {
		return 0;
	}
	k_thread_create(&bridge_thread, bridge_thread_stack,
				K_THREAD_STACK_SIZEOF(bridge_thread_stack),
				bridge_thread_fn, NULL, NULL, NULL,
				K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	started = true;
	return 0;
}

#if defined(CONFIG_RADIO_BRIDGE_VALIDATION_CDC)
int bridge_runtime_validation_inject(size_t len, uint8_t seed)
{
	int ret;
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	ret = rb_validation_inject_pattern(&validation_pipe, len, seed);
	k_spin_unlock(&validation_lock, key);
	if (ret == 0) {
		bridge_thread_wake();
	}
	return ret;
}

int bridge_runtime_validation_verify(size_t len, uint8_t seed,
				     size_t *mismatch_offset)
{
	int ret;
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	ret = rb_validation_verify_output(&validation_pipe, len, seed,
						 mismatch_offset);
	k_spin_unlock(&validation_lock, key);
	return ret;
}

size_t bridge_runtime_validation_copy(size_t offset, uint8_t *data,
				      size_t max_len)
{
	size_t copied;
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	copied = rb_validation_copy_output(&validation_pipe, offset, data, max_len);
	k_spin_unlock(&validation_lock, key);
	return copied;
}

void bridge_runtime_validation_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	rb_validation_clear(&validation_pipe);
	k_spin_unlock(&validation_lock, key);
}

void bridge_runtime_validation_stats_get(struct rb_validation_stats *stats)
{
	k_spinlock_key_t key = k_spin_lock(&validation_lock);

	rb_validation_stats_get(&validation_pipe, stats);
	k_spin_unlock(&validation_lock, key);
}

int bridge_runtime_validation_time_pair(int64_t utc_seconds)
{
	k_spinlock_key_t key;

	if (!initialized || !scheduler.config.master ||
	    runtime_time_source_mode != 1u || utc_seconds < 0) {
		return -EACCES;
	}
	key = k_spin_lock(&validation_lock);
	if (validation_time_command != 0u) {
		k_spin_unlock(&validation_lock, key);
		return -EBUSY;
	}
	validation_pair_seconds = utc_seconds;
	validation_time_command = 1u;
	k_spin_unlock(&validation_lock, key);
	bridge_thread_wake();
	return 0;
}

int bridge_runtime_validation_time_source_lost(void)
{
	k_spinlock_key_t key;

	if (!initialized || !scheduler.config.master ||
	    runtime_time_source_mode != 1u) {
		return -EACCES;
	}
	key = k_spin_lock(&validation_lock);
	if (validation_time_command != 0u) {
		k_spin_unlock(&validation_lock, key);
		return -EBUSY;
	}
	validation_time_command = 2u;
	k_spin_unlock(&validation_lock, key);
	bridge_thread_wake();
	return 0;
}
#endif

void bridge_runtime_stats_get(struct rb_bridge_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	*stats = bridge_stats;
	stats->active_count = rb_scheduler_active_count(&scheduler);
	stats->suspect_count = rb_membership_suspect_count(&scheduler.membership);
	stats->queue_drop_bytes = rb_scheduler_queue_drop_bytes(&scheduler);
	stats->duplicate_packets = rb_scheduler_duplicate_count(&scheduler);
	stats->invalid_session_packets = rb_scheduler_invalid_session_count(&scheduler);
	stats->sync_state = (uint8_t)sync_filter_state(&sync_filter_runtime);
	stats->sync_age_us = sync_filter_runtime.age_us;
	stats->slave_active = scheduler.slave_active ? 1u : 0u;
	stats->slave_node_id = scheduler.slave_node_id;
}

enum sync_filter_state bridge_runtime_sync_state(void)
{
	return sync_filter_state(&sync_filter_runtime);
}

int64_t bridge_runtime_sync_offset_us(void)
{
	return sync_filter_offset_us(&sync_filter_runtime);
}

int32_t bridge_runtime_sync_drift_ppm(void)
{
	return sync_filter_drift_ppm(&sync_filter_runtime);
}
