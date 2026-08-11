cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(time_uart_logic_tests)

target_include_directories(app PRIVATE
	../../../src/bridge
	../../../src/time
)
target_sources(app PRIVATE
	src/test_time_uart.c
	src/test_uart_rx_recovery.c
	../../../src/bridge/byte_ring.c
	../../../src/bridge/uart_rx_recovery.c
	../../../src/time/time_uart.c
)
