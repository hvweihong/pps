#include "nmea_parser.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static int hex_value(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static int decimal(const char *s, size_t n, unsigned *value)
{
	unsigned v = 0;
	if (!s || !value || n == 0) return -EINVAL;
	for (size_t i = 0; i < n; ++i) {
		if (s[i] < '0' || s[i] > '9' || v > (UINT_MAX - (unsigned)(s[i] - '0')) / 10u)
			return -ERANGE;
		v = v * 10u + (unsigned)(s[i] - '0');
	}
	*value = v;
	return 0;
}

static int parse_time(const char *s, size_t n, struct rb_nmea_utc *utc)
{
	unsigned hh, mm, ss;
	size_t whole = n;
	const char *dot = memchr(s, '.', n);
	if (dot) whole = (size_t)(dot - s);
	if (whole != 6 || decimal(s, 2, &hh) || decimal(s + 2, 2, &mm) || decimal(s + 4, 2, &ss))
		return -EINVAL;
	if (hh > 23 || mm > 59 || ss > 59) return -ERANGE;
	utc->hour = (uint8_t)hh;
	utc->minute = (uint8_t)mm;
	utc->second = (uint8_t)ss;
	utc->millisecond = 0;
	if (dot) {
		size_t fraction = n - whole - 1;
		unsigned ms = 0;
		if (fraction == 0) return -EINVAL;
		for (size_t i = 0; i < fraction && i < 3; ++i) {
			if (s[whole + 1 + i] < '0' || s[whole + 1 + i] > '9') return -EINVAL;
			ms = ms * 10u + (unsigned)(s[whole + 1 + i] - '0');
		}
		for (size_t i = 3; i < fraction; ++i)
			if (s[whole + 1 + i] < '0' || s[whole + 1 + i] > '9') return -EINVAL;
		if (fraction == 1) ms *= 100;
		else if (fraction == 2) ms *= 10;
		utc->millisecond = (uint16_t)ms;
	}
	return 0;
}

static int parse_date(const char *s, size_t n, struct rb_nmea_utc *utc)
{
	unsigned day, month, yy;
	if (n != 6 || decimal(s, 2, &day) || decimal(s + 2, 2, &month) || decimal(s + 4, 2, &yy))
		return -EINVAL;
	utc->day = (uint8_t)day;
	utc->month = (uint8_t)month;
	utc->year = (uint16_t)(yy >= 80 ? 1900 + yy : 2000 + yy);
	return 0;
}

static bool leap(unsigned year)
{
	return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static int validate_calendar(const struct rb_nmea_utc *u)
{
	static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	if (!u || u->year == 0 || u->month < 1 || u->month > 12 || u->day < 1 ||
	    u->hour > 23 || u->minute > 59 || u->second > 59) return -ERANGE;
	uint8_t max = days[u->month - 1];
	if (u->month == 2 && leap(u->year)) max = 29;
	return u->day <= max ? 0 : -ERANGE;
}

static int field(const char *body, size_t body_len, unsigned index,
		 const char **start, size_t *length)
{
	const char *p = body, *end = body + body_len;
	for (unsigned i = 0; i < index; ++i) {
		p = memchr(p, ',', (size_t)(end - p));
		if (!p) return -EINVAL;
		++p;
	}
	const char *comma = memchr(p, ',', (size_t)(end - p));
	*start = p;
	*length = comma ? (size_t)(comma - p) : (size_t)(end - p);
	return 0;
}

int rb_nmea_parse_sentence(const char *line, size_t len, struct rb_nmea_utc *utc)
{
	if (!line || !utc || len == 0) return -EINVAL;
	if (len > RB_NMEA_MAX_SENTENCE_LENGTH) return -EMSGSIZE;
	if (line[0] != '$') return -EINVAL;
	while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
	if (len < 7 || line[1] == '\0') return -EINVAL;
	const char *star = memchr(line + 1, '*', len - 1);
	if (!star || (size_t)(star - line) + 3 != len) return -EBADMSG;
	int h1 = hex_value(star[1]), h2 = hex_value(star[2]);
	if (h1 < 0 || h2 < 0) return -EBADMSG;
	unsigned checksum = 0;
	for (const char *p = line + 1; p < star; ++p) checksum ^= (unsigned char)*p;
	if (checksum != (unsigned)((h1 << 4) | h2)) return -EBADMSG;
	const char *body = line + 1;
	size_t body_len = (size_t)(star - body);
	const char *type;
	size_t type_len;
	if (field(body, body_len, 0, &type, &type_len) != 0 || type_len != 5) return -ENOTSUP;
	bool rmc = memcmp(type + 2, "RMC", 3) == 0;
	bool zda = memcmp(type + 2, "ZDA", 3) == 0;
	if (!rmc && !zda) return -ENOTSUP;
	const char *s;
	size_t n;
	int ret = field(body, body_len, 1, &s, &n);
	if (ret != 0) return ret;
	ret = parse_time(s, n, utc);
	if (ret != 0) return ret;
	if (rmc) {
		if (field(body, body_len, 2, &s, &n) != 0 || n != 1) return -EINVAL;
		if (*s == 'V') return -EAGAIN;
		if (*s != 'A') return -EAGAIN;
		ret = field(body, body_len, 9, &s, &n);
		if (ret != 0) return ret;
		ret = parse_date(s, n, utc);
		if (ret != 0) return ret;
	} else {
		unsigned day, month, year;
		if (field(body, body_len, 2, &s, &n) != 0 || decimal(s, n, &day) != 0 || day > 31) return -ERANGE;
		utc->day = (uint8_t)day;
		if (field(body, body_len, 3, &s, &n) != 0 || decimal(s, n, &month) != 0 || month > 12) return -ERANGE;
		utc->month = (uint8_t)month;
		if (field(body, body_len, 4, &s, &n) != 0 || decimal(s, n, &year) != 0 || year > UINT16_MAX) return -ERANGE;
		utc->year = (uint16_t)year;
	}
	return validate_calendar(utc);
}

int rb_nmea_utc_to_unix(const struct rb_nmea_utc *u, int64_t *seconds)
{
	if (!u || !seconds || validate_calendar(u) != 0 || u->year < 1970) return -ERANGE;
	int64_t days = 0;
	for (unsigned y = 1970; y < u->year; ++y) days += leap(y) ? 366 : 365;
	static const uint16_t before[] = {0,31,59,90,120,151,181,212,243,273,304,334};
	days += before[u->month - 1] + u->day - 1;
	if (u->month > 2 && leap(u->year)) ++days;
	*seconds = days * 86400 + u->hour * 3600 + u->minute * 60 + u->second;
	return 0;
}
