cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_nrf52840_plus_time_sync)

target_include_directories(app PRIVATE src)
target_sources(app PRIVATE
	src/main.c
	src/ble_time_sync.c
	src/ble_time_sync_client.c
	src/ble_time_sync_service.c
	src/ble_time_sync_uuids.c
	src/time_sync_math.c
	src/runtime_config.c
	src/sync_packet.c
	src/sync_filter.c
	src/timebase.c
	src/pps_output.c
	src/radio_sync.c
	src/status.c
	src/heartbeat_led.c
)
