cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sync_logic_tests)

target_include_directories(app PRIVATE ../../src)
target_sources(app PRIVATE
	src/main.c
	../../src/sync_filter.c
	../../src/time_sync_math.c
)
