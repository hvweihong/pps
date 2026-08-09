#include "utc_clock.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define RB_UTC_PPS_PERIOD_US 1000000ULL
#define RB_UTC_PAIR_MAX_AGE_US 900000ULL
#define RB_UTC_MIN_EDGE_GAP_US 500000ULL

static int schedule_reset(struct rb_utc_clock *c, uint64_t pps_tick, int64_t sec)
{
	uint64_t target;
	int64_t next_second;

	if (pps_tick > UINT64_MAX - RB_UTC_PPS_PERIOD_US || sec == INT64_MAX)
		return -ERANGE;
	target = pps_tick + RB_UTC_PPS_PERIOD_US;
	next_second = sec + 1;
	if (c->last_output_tick) {
		if (c->last_output_tick > UINT64_MAX - RB_UTC_MIN_EDGE_GAP_US)
			return -ERANGE;
		if (target < c->last_output_tick + RB_UTC_MIN_EDGE_GAP_US) {
			if (target > UINT64_MAX - RB_UTC_PPS_PERIOD_US ||
			    next_second == INT64_MAX)
				return -ERANGE;
		target += RB_UTC_PPS_PERIOD_US;
		++next_second;
		}
	}
	if (c->config.phase_reset) {
		int ret = c->config.phase_reset(c->config.phase_reset_context, target);
		if (ret != 0) return ret;
	}
	c->next_edge_tick = target;
	c->next_edge_second = next_second;
	return 0;
}

void rb_utc_clock_init(struct rb_utc_clock *c, const struct rb_utc_clock_config *cfg)
{
	if (!c) return;
	memset(c, 0, sizeof(*c));
	c->config = cfg ? *cfg : (struct rb_utc_clock_config){ .external_mode = false, .loss_timeout_us = 3000000 };
	if (c->config.loss_timeout_us == 0) c->config.loss_timeout_us = 3000000;
	c->state = c->config.external_mode ? RB_UTC_ACQUIRING : RB_UTC_LOCAL;
}

int rb_utc_clock_note_pair(struct rb_utc_clock *c, uint64_t pps_tick, int64_t sec)
{
	bool discontinuous;
	bool reset;
	uint8_t valid_pairs;
	int ret;

	if (!c) return -EINVAL;
	if (!c->config.external_mode) return -EACCES;
	if (c->now_tick < pps_tick || c->now_tick - pps_tick > RB_UTC_PAIR_MAX_AGE_US)
		return -ERANGE;
	if (c->have_pair && (pps_tick <= c->last_pair_tick || sec <= c->last_pair_second))
		return -ERANGE;
	discontinuous = c->have_pair && c->state != RB_UTC_HOLDOVER &&
		(pps_tick - c->last_pair_tick != RB_UTC_PPS_PERIOD_US ||
		 c->last_pair_second == INT64_MAX || sec != c->last_pair_second + 1);

	reset = !c->have_pair || c->state == RB_UTC_HOLDOVER ||
		c->state == RB_UTC_INVALID || discontinuous;
	if (reset) valid_pairs = 1;
	else if (c->state == RB_UTC_LOCKED) valid_pairs = 3;
	else if (c->valid_pairs == UINT8_MAX) return -ERANGE;
	else valid_pairs = c->valid_pairs + 1;
	if (reset) {
		ret = schedule_reset(c, pps_tick, sec);
		if (ret != 0) return ret;
	}
	c->last_pair_tick = pps_tick;
	c->last_pair_second = sec;
	c->have_pair = true;
	c->valid_pairs = valid_pairs;
	if (c->state != RB_UTC_LOCKED || reset) {
		if (c->valid_pairs >= 3) c->state = RB_UTC_LOCKED;
		else c->state = RB_UTC_ACQUIRING;
	}
	return 0;
}

void rb_utc_clock_tick(struct rb_utc_clock *c, uint64_t now_tick)
{
	if (!c) return;
	if (now_tick < c->now_tick) return;
	c->now_tick = now_tick;
	if (c->next_edge_tick && now_tick >= c->next_edge_tick) {
		uint64_t count = (now_tick - c->next_edge_tick) / RB_UTC_PPS_PERIOD_US + 1;
		uint64_t tick_step;

		if (count > UINT64_MAX / RB_UTC_PPS_PERIOD_US ||
		    count > INT64_MAX ||
		    c->next_edge_tick > UINT64_MAX - count * RB_UTC_PPS_PERIOD_US ||
		    c->next_edge_second > INT64_MAX - (int64_t)count) {
			c->state = RB_UTC_INVALID;
			c->next_edge_tick = 0;
			return;
		}
		tick_step = count * RB_UTC_PPS_PERIOD_US;
		c->last_output_tick = c->next_edge_tick + (count - 1) * RB_UTC_PPS_PERIOD_US;
		c->next_edge_tick += tick_step;
		c->next_edge_second += (int64_t)count;
	}
	if (c->have_pair && now_tick >= c->last_pair_tick &&
	    now_tick - c->last_pair_tick >= c->config.loss_timeout_us &&
	    (c->state == RB_UTC_ACQUIRING || c->state == RB_UTC_LOCKED))
		c->state = RB_UTC_HOLDOVER;
}

int rb_utc_clock_publication(const struct rb_utc_clock *c, uint64_t now_tick,
			     struct rb_utc_publication *p)
{
	struct rb_utc_clock projected;

	if (!c || !p) return -EINVAL;
	if (now_tick < c->now_tick) return -ERANGE;
	projected = *c;
	rb_utc_clock_tick(&projected, now_tick);
	p->next_pps_utc_seconds = projected.next_edge_second;
	p->quality = RB_TIME_UTC_INVALID;
	p->valid = false;
	if (projected.state == RB_UTC_LOCKED) { p->quality = RB_TIME_LOCKED; p->valid = true; }
	else if (projected.state == RB_UTC_HOLDOVER) { p->quality = RB_TIME_HOLDOVER; p->valid = true; }
	return 0;
}

enum rb_utc_state rb_utc_clock_state(const struct rb_utc_clock *c)
{
	return c ? c->state : RB_UTC_INVALID;
}
