#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "nmea_parser.h"

ZTEST(nmea_parser, test_rmc_active_is_decoded)
{
	struct rb_nmea_utc utc;
	const char *sentence =
		"$GPRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230394,,,A*5E\r\n";
	zassert_ok(rb_nmea_parse_sentence(sentence, strlen(sentence), &utc));
	zassert_equal(utc.year, 1994);
	zassert_equal(utc.month, 3);
	zassert_equal(utc.day, 23);
	zassert_equal(utc.hour, 12);
	zassert_equal(utc.minute, 35);
	zassert_equal(utc.second, 19);
	zassert_equal(utc.millisecond, 0);
}

ZTEST(nmea_parser, test_zda_is_decoded)
{
	struct rb_nmea_utc utc;
	const char *sentence = "$GPZDA,201530.00,04,07,2002,00,00*60\r\n";
	zassert_ok(rb_nmea_parse_sentence(sentence, strlen(sentence), &utc));
	zassert_equal(utc.year, 2002);
	zassert_equal(utc.month, 7);
	zassert_equal(utc.day, 4);
	zassert_equal(utc.hour, 20);
	zassert_equal(utc.minute, 15);
	zassert_equal(utc.second, 30);
	zassert_equal(utc.millisecond, 0);
}

ZTEST(nmea_parser, test_bad_checksum_and_void_rmc_are_rejected)
{
	struct rb_nmea_utc utc;
	const char *bad = "$GPRMC,123519.00,A,4807.038,N,01131.000,E,0,0,230394,,,A*00\r\n";
	const char *void_fix = "$GPRMC,123519.00,V,,,,,,,230394,,,N*7F\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad, strlen(bad), &utc), -EBADMSG);
	zassert_equal(rb_nmea_parse_sentence(void_fix, strlen(void_fix), &utc), -EAGAIN);
}

ZTEST(nmea_parser, test_boundaries_fraction_leap_and_conversion)
{
	struct rb_nmea_utc utc;
	int64_t seconds;
	const char *leap = "$GPZDA,235959.999,29,02,2024,00,00*53\r\n";
	zassert_ok(rb_nmea_parse_sentence(leap, strlen(leap), &utc));
	zassert_equal(utc.millisecond, 999);
	zassert_ok(rb_nmea_utc_to_unix(&utc, &seconds));
	zassert_equal(seconds, 1709251199);

	const char *bad_date = "$GPZDA,120000,29,02,2023,00,00*41\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad_date, strlen(bad_date), &utc), -ERANGE);
	const char *bad_time = "$GPZDA,246000,01,01,2024,00,00*4C\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad_time, strlen(bad_time), &utc), -ERANGE);
	const char *bad_fraction = "$GPZDA,1200.00,01,01,2024,00,00*61\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad_fraction, strlen(bad_fraction), &utc), -EINVAL);
}

ZTEST(nmea_parser, test_zda_requires_fixed_width_date_fields)
{
	struct rb_nmea_utc utc;
	const char *short_day = "$GPZDA,201530.00,4,07,2002,00,00*50\r\n";
	const char *short_month = "$GPZDA,201530.00,04,7,2002,00,00*50\r\n";
	const char *long_year = "$GPZDA,201530.00,04,07,02002,00,00*50\r\n";
	zassert_equal(rb_nmea_parse_sentence(short_day, strlen(short_day), &utc), -EINVAL);
	zassert_equal(rb_nmea_parse_sentence(short_month, strlen(short_month), &utc), -EINVAL);
	zassert_equal(rb_nmea_parse_sentence(long_year, strlen(long_year), &utc), -EINVAL);
}

ZTEST(nmea_parser, test_failure_does_not_mutate_utc_output)
{
	struct rb_nmea_utc utc = {1999, 9, 9, 9, 9, 9, 999};
	const struct rb_nmea_utc expected = {1999, 9, 9, 9, 9, 9, 999};
	const char *bad_date = "$GPZDA,120000,29,02,2023,00,00*41\r\n";
	zassert_equal(rb_nmea_parse_sentence(bad_date, strlen(bad_date), &utc), -ERANGE);
	zassert_mem_equal(&utc, &expected, sizeof(utc));
}

ZTEST(nmea_parser, test_missing_dollar_star_unsupported_and_overlong_rejected)
{
	struct rb_nmea_utc utc;
	const char *missing_dollar = "GPZDA,201530.00,04,07,2002,00,00*4E";
	const char *missing_star = "$GPZDA,201530.00,04,07,2002,00,00";
	const char *unsupported = "$GPTXT,01,01,02,hello*2F";
	char overlong[RB_NMEA_MAX_SENTENCE_LENGTH + 1];
	memset(overlong, 'A', sizeof(overlong));
	zassert_equal(rb_nmea_parse_sentence(missing_dollar, strlen(missing_dollar), &utc), -EINVAL);
	zassert_equal(rb_nmea_parse_sentence(missing_star, strlen(missing_star), &utc), -EBADMSG);
	zassert_equal(rb_nmea_parse_sentence(unsupported, strlen(unsupported), &utc), -ENOTSUP);
	zassert_equal(rb_nmea_parse_sentence(overlong, sizeof(overlong), &utc), -EMSGSIZE);
}

ZTEST(nmea_parser, test_epoch_and_pre_epoch_conversion)
{
	struct rb_nmea_utc utc = {1970, 1, 1, 0, 0, 0, 0};
	int64_t seconds;
	zassert_ok(rb_nmea_utc_to_unix(&utc, &seconds));
	zassert_equal(seconds, 0);
	utc.year = 1969;
	zassert_equal(rb_nmea_utc_to_unix(&utc, &seconds), -ERANGE);
}

ZTEST_SUITE(nmea_parser, NULL, NULL, NULL, NULL, NULL);
