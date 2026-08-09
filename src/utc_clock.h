#ifndef RB_UTC_CLOCK_H
#define RB_UTC_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

enum rb_utc_state { RB_UTC_LOCAL, RB_UTC_ACQUIRING, RB_UTC_LOCKED,
	RB_UTC_HOLDOVER, RB_UTC_INVALID };
enum rb_time_quality { RB_TIME_UTC_INVALID, RB_TIME_LOCKED, RB_TIME_HOLDOVER };

typedef int (*rb_utc_phase_reset_fn)(void *context, uint64_t target_tick);

struct rb_utc_clock_config {
	bool external_mode;
	uint32_t loss_timeout_us;
	rb_utc_phase_reset_fn phase_reset;
	void *phase_reset_context;
};

struct rb_utc_publication {
	int64_t next_pps_utc_seconds;
	enum rb_time_quality quality;
	bool valid;
};

struct rb_utc_clock {
	struct rb_utc_clock_config config;
	enum rb_utc_state state;
	uint64_t now_tick;
	uint64_t last_pair_tick;
	int64_t last_pair_second;
	uint64_t next_edge_tick;
	int64_t next_edge_second;
	uint64_t last_output_tick;
	uint8_t valid_pairs;
	bool have_pair;
};

void rb_utc_clock_init(struct rb_utc_clock *clock,
			       const struct rb_utc_clock_config *config);
int rb_utc_clock_note_pair(struct rb_utc_clock *clock,
			   uint64_t pps_tick, int64_t utc_second);
void rb_utc_clock_tick(struct rb_utc_clock *clock, uint64_t now_tick);
int rb_utc_clock_publication(const struct rb_utc_clock *clock,
			     uint64_t now_tick,
			     struct rb_utc_publication *publication);
enum rb_utc_state rb_utc_clock_state(const struct rb_utc_clock *clock);

#endif
