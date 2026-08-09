#ifndef RB_NMEA_PARSER_H
#define RB_NMEA_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define RB_NMEA_MAX_SENTENCE_LENGTH 82u

struct rb_nmea_utc {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint16_t millisecond;
};

int rb_nmea_parse_sentence(const char *line, size_t len,
				   struct rb_nmea_utc *utc);
int rb_nmea_utc_to_unix(const struct rb_nmea_utc *utc, int64_t *seconds);

#endif
