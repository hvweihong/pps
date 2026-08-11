cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sync_logic_tests)

target_include_directories(app PRIVATE
	../../../src/app
	../../../src/bridge
	../../../src/time
)
target_sources(app PRIVATE
	src/test_time_sync.c
	src/test_nmea_parser.c
	src/test_utc_clock.c
	src/test_sync_tracker.c
	src/test_wireless_time_sync.c
	../../../src/time/sync_filter.c
	../../../src/time/time_sync_math.c
	../../../src/time/nmea_parser.c
	../../../src/time/utc_clock.c
	../../../src/time/sync_tracker.c
	../../../src/time/wireless_time_sync.c
)
