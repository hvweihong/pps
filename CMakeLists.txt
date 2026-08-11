cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_nrf52840_plus_time_sync)

# Fail during configure instead of waiting for the firmware compiler/runtime to
# discover a malformed group key.  Kconfig exports string symbols as CMake
# variables after find_package(Zephyr).
if(DEFINED CONFIG_RADIO_BRIDGE_GROUP_KEY)
	string(LENGTH "${CONFIG_RADIO_BRIDGE_GROUP_KEY}" RB_GROUP_KEY_HEX_LENGTH)
	if(NOT RB_GROUP_KEY_HEX_LENGTH EQUAL 32 OR
	   NOT CONFIG_RADIO_BRIDGE_GROUP_KEY MATCHES "^[0-9A-Fa-f]+$")
		message(FATAL_ERROR
			"CONFIG_RADIO_BRIDGE_GROUP_KEY must contain exactly 32 hexadecimal characters")
	endif()
endif()

target_include_directories(app PRIVATE
	src/app
	src/config
	src/bridge
	src/radio
	src/time
	src/validation
)

target_sources(app PRIVATE
	src/config/bridge_config.c
	src/config/param_config.c
	src/config/param_shell.c
	src/bridge/byte_ring.c
	src/bridge/record_queue.c
	src/bridge/link_protocol.c
	src/bridge/fixed_peer_table.c
	src/bridge/link_scheduler_core.c
	src/bridge/uart_bridge.c
	src/bridge/uart_rx_recovery.c
	src/bridge/bridge_runtime.c
	src/radio/radio_address.c
	src/radio/radio_address_crypto.c
	src/radio/radio_transport_policy.c
	src/radio/radio_transport.c
	src/time/wireless_time_sync.c
	src/time/sync_tracker.c
	src/time/sync_filter.c
	src/time/time_sync_math.c
	src/time/timebase.c
	src/time/pps_output.c
	src/time/pps_input.c
	src/time/time_uart.c
	src/time/nmea_parser.c
	src/time/utc_clock.c
	src/time/time_sync_shell.c
	src/app/status.c
	src/app/heartbeat_led.c
	src/app/boot_recovery.c
	src/app/main.c
)

target_sources_ifdef(CONFIG_RADIO_BRIDGE_VALIDATION_CDC app PRIVATE
	src/validation/bridge_validation.c
	src/validation/bridge_validation_shell.c
	src/validation/boot_validation_shell.c
)

set(APP_GIT_VERSION "unknown")
find_package(Git QUIET)
if(Git_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE APP_GIT_SHA
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD --
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		RESULT_VARIABLE APP_GIT_DIRTY_RESULT
		ERROR_QUIET
	)
	if(APP_GIT_SHA)
		set(APP_GIT_VERSION "${APP_GIT_SHA}")
		if(APP_GIT_DIRTY_RESULT EQUAL 1)
			set(APP_GIT_VERSION "${APP_GIT_VERSION}-dirty")
		endif()
	endif()
endif()

target_compile_definitions(app PRIVATE
	APP_GIT_VERSION="${APP_GIT_VERSION}"
	APP_LOG_FORMAT_VERSION=2
)
