cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(config_tests)

target_include_directories(app PRIVATE
	../../../src/config
	../../../src/bridge
)
target_compile_definitions(app PRIVATE
	CONFIG_RADIO_BRIDGE_GROUP_ID=1
	CONFIG_RADIO_BRIDGE_GROUP_KEY="00112233445566778899aabbccddeeff"
	CONFIG_RADIO_BRIDGE_UART_BAUDRATE=115200
	CONFIG_TIME_SYNC_RADIO_DELAY_US=0
	CONFIG_TIME_UART_BAUDRATE=9600
)
target_sources(app PRIVATE
	src/test_config.c
	src/test_param_config.c
	../../../src/config/bridge_config.c
	../../../src/config/param_config.c
)
