#ifndef PPS_OUTPUT_H
#define PPS_OUTPUT_H

#include <stdint.h>

struct pps_output_stats {
	uint64_t last_rise_tick;
	uint64_t scheduled_rise_tick;
	uint64_t epoch_tick;
	uint32_t late_schedules;
	uint32_t pulses;
	uint32_t phase_resets;
};

int pps_output_init(uint32_t pulse_width_us);
int pps_output_schedule(uint64_t rise_tick);
int pps_output_start_periodic(uint64_t first_rise_tick, uint32_t period_us);
int pps_output_reset_epoch(uint64_t first_rise_tick);
uint64_t pps_output_scheduled_tick(void);
const struct pps_output_stats *pps_output_stats_get(void);

#endif
