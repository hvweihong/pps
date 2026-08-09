cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bridge_logic_tests)

target_include_directories(app PRIVATE ../../src)
target_compile_definitions(app PRIVATE
	CONFIG_RADIO_BRIDGE_GROUP_ID=1
	CONFIG_RADIO_BRIDGE_GROUP_KEY="00112233445566778899aabbccddeeff"
	CONFIG_TIME_SYNC_PPS_PERIOD_US=1000000
	CONFIG_TIME_SYNC_PPS_WIDTH_US=100
	CONFIG_TIME_SYNC_RADIO_DELAY_US=0
	CONFIG_TIME_SYNC_STATUS_INTERVAL_MS=1000
)
target_sources(app PRIVATE
	src/test_config.c
	src/test_link_protocol.c
	src/test_byte_ring.c
	src/test_bridge_validation.c
	src/test_link_window.c
	src/test_membership.c
	src/test_sync_tracker.c
	src/test_radio_address.c
	src/test_wireless_time_sync.c
	src/test_master_scheduler.c
	src/test_slave_scheduler.c
	src/test_param_config.c
	../../src/bridge_config.c
	../../src/link_protocol.c
	../../src/byte_ring.c
	../../src/bridge_validation.c
	../../src/link_window.c
	../../src/membership.c
	../../src/sync_tracker.c
	../../src/radio_address.c
	../../src/wireless_time_sync.c
	../../src/link_scheduler_core.c
	../../src/param_config.c
)
