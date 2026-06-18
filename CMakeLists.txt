cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_nrf52840_plus_time_sync)

target_include_directories(app PRIVATE src)

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
