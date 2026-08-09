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

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
	src/bridge_config.c
	src/byte_ring.c
	src/record_queue.c
	src/link_protocol.c
	src/link_window.c
	src/membership.c
	src/link_scheduler_core.c
	src/radio_address.c
	src/radio_address_crypto.c
	src/radio_transport.c
	src/uart_bridge.c
	src/wireless_time_sync.c
	src/sync_tracker.c
	src/sync_filter.c
	src/time_sync_math.c
	src/timebase.c
	src/pps_output.c
	src/pps_input.c
	src/time_uart.c
	src/nmea_parser.c
	src/utc_clock.c
	src/param_config.c
	src/param_shell.c
	src/bridge_runtime.c
	src/time_sync_shell.c
	src/status.c
	src/heartbeat_led.c
)

target_sources_ifdef(CONFIG_RADIO_BRIDGE_VALIDATION_CDC app PRIVATE
	src/bridge_validation.c
	src/bridge_validation_shell.c
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

target_sources(app PRIVATE src/main.c)
