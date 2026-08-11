cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bridge_logic_tests)

target_include_directories(app PRIVATE
	../../../src/app
	../../../src/config
	../../../src/bridge
	../../../src/radio
	../../../src/time
	../../../src/validation
)
target_compile_definitions(app PRIVATE
	CONFIG_RADIO_BRIDGE_GROUP_ID=1
	CONFIG_RADIO_BRIDGE_GROUP_KEY="00112233445566778899aabbccddeeff"
	CONFIG_RADIO_BRIDGE_UART_BAUDRATE=115200
	CONFIG_RADIO_BRIDGE_UART_RING_SIZE=16384
	CONFIG_RADIO_BRIDGE_AGGREGATION_TIMEOUT_US=1000
	CONFIG_RADIO_BRIDGE_SYNC_INTERVAL_US=100000
	CONFIG_RADIO_BRIDGE_LEASE_TIMEOUT_US=100000
	CONFIG_RADIO_BRIDGE_IDLE_POLL_MAX_US=5000
	CONFIG_TIME_SYNC_PPS_WIDTH_US=100000
	CONFIG_TIME_SYNC_RADIO_DELAY_US=0
	CONFIG_TIME_SYNC_STATUS_INTERVAL_MS=10000
	CONFIG_TIME_UART_BAUDRATE=9600
)
target_sources(app PRIVATE
	src/test_link_protocol.c
	src/test_byte_ring.c
	src/test_record_queue.c
	src/test_bridge_validation.c
	src/test_bridge_runtime_policy.c
	src/test_fixed_peer_table.c
	src/test_master_scheduler.c
	src/test_slave_scheduler.c
	../../../src/bridge/link_protocol.c
	../../../src/bridge/byte_ring.c
	../../../src/bridge/record_queue.c
	../../../src/validation/bridge_validation.c
	../../../src/bridge/fixed_peer_table.c
	../../../src/bridge/link_scheduler_core.c
)
