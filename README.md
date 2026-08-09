# XIAO nRF52840 Plus 无线 PPS 与 UART Bridge

这是一个运行在 Seeed XIAO nRF52840 Plus 上的 Zephyr/NCS 固件。同一份固件可通过
NVS 配置为 master 或 slave，并在一个 ESB 星型网络内同时提供：

- 1 Hz PPS 时间同步；
- master 本地授时或外部 NMEA/PPS 驯服；
- master 到多个 slave 的 best-effort 广播下行；
- 每个 slave 到 master 的独立、可靠轮询上行；
- 物理 UART 数据桥，以及 validation 固件中的 CDC 端到端注入/校验；
- NVS 参数、分组隔离、watchdog 和冷启动保护。

当前板级验证使用：

- master：USB ID `DBE5C3D84EA2EC6F`，`role_id=0`；
- slave：USB ID `5B3D71D27A709CA2`，`role_id=1`；
- `group_id=1`；
- `group_key=00112233445566778899aabbccddeeff`；
- 数据 UART：115200 baud。

完整阶段证据见 `docs/verification/incremental-board-e2e-results.md`。

## 系统拓扑

```text
                         best-effort broadcast
                    +----------------------------+
                    |                            v
host UART <-> master board                 slave board <-> host UART
                    ^                            |
                    +------ polled uplink -------+
                           per-slave records
```

- master 下行不保留 repair history，也不等待业务 ACK；丢包后 slave 直接接受后续序号。
- slave 上行通过 master 轮询和 ACK 确认；未确认的数据会重发。
- master 为每个 node 保留独立 record queue，并按轮询顺序整条输出到 UART，避免不同
  slave 的记录被拼接成一个不可区分的字节流。
- slave 之间没有直接业务数据路径。
- 当前最大活动 slave 数为 3。

## PPS 与时间源

所有板使用 1 MHz TIMER3 作为统一 64-bit 微秒 timebase。PPS 输出由 TIMER3 compare、
GPIOTE 和 GPPI/PPI 直接产生，不依赖线程 sleep 或软件 GPIO 翻转。

| 项目 | 当前行为 |
| --- | --- |
| PPS 输出 | P0.03，1 Hz，高电平有效 |
| PPS 默认脉宽 | 100000 us（100 ms） |
| timebase 分辨率 | 1 us |
| master 本地模式 | `time_source_mode=0`，以本地 timebase 对外同步，UTC 标记无效 |
| master 外部模式 | `time_source_mode=1`，组合 P0.02 PPS 与 UART1 NMEA RMC/ZDA |
| slave 时间源 | 无线同步 publication |

外部模式在连续 3 组有效 PPS/NMEA 配对后进入 `EXTERNAL_LOCKED`。任一输入连续约 3 秒
缺失后进入 `HOLDOVER`，继续基于最后的 UTC 基准和本地 timebase 运行。外部模式冷启动、
尚未取得有效配对时为 `EXTERNAL_ACQUIRING/UTC_INVALID`。

slave 通过连续无线同步帧估计 master/slave TIMER3 的 offset，并把本地 PPS 上升沿调度到
master PPS epoch。无线状态可通过 `time_sync status` 和周期状态日志查看。

当前双板验证已证明 PPS scheduler 持续以 1 Hz/100 ms 配置运行，以及 slave 可恢复到
`LOCKED`。真实外部 GNSS/PPS 捕获、PPS 电气脉宽和锁定后 `<=20 us` 相位误差仍必须使用
GNSS/PPS 源与示波器完成最终验收，不能由 CDC synthetic pair 替代。

## UART 接线

| 用途 | 板端 TX | 板端 RX | 默认波特率 | 说明 |
| --- | --- | --- | --- | --- |
| 数据 UART bridge (`uart0`) | P1.11 | P1.12 | 115200 | master/slave 双向原始数据 |
| 外部时间 UART (`uart1`) | P0.04 | P0.05 | 9600 | external master 的 NMEA 输入 |
| PPS 输出 | P0.03 | - | 1 Hz | 高电平有效，默认 100 ms |
| PPS 输入 | - | P0.02 | 1 Hz | 仅 external master 初始化 |
| USB CDC | USB | USB | 115200 line coding | shell、日志、validation bridge |

外部 UART 必须共地，并交叉连接：

```text
board TX -> adapter RX
board RX <- adapter TX
board GND -- adapter GND
```

数据 UART 使用 8N1、无硬件流控。`uart_baudrate` 和 `time_uart_baudrate` 在启动时应用，
修改后需要 cold reboot。

本轮物理 UART fixture 已完成双向端到端验证：`/dev/ttyACM2 -> master -> wireless ->
slave -> /dev/ttyS1` 与反方向均精确传输 600/600 bytes，SHA-256 一致，板端 UART
record/TX 全部完成且无 drop/start-error。旧 Android adb daemon 仅支持 PTY，因此测试工具在
adb 边界使用 base64 编解码，避免 PTY 改写原始二进制字节。

## 参数与持久化

参数优先级固定为：

1. NVS 中存在有效持久化值时使用持久化值；
2. NVS 未配置、对应 key 被清除、backend 不可用或 load 失败时使用编译默认值。

关键参数：

| 参数 | 默认值 | 范围/含义 | 生效方式 |
| --- | ---: | --- | --- |
| `role_id` | 0 | 0=master，1..3=slave | reboot |
| `group_id` | 1 | 1..0xFFFFFFFE | reboot |
| `group_key` | `001122...eeff` | 16-byte hex | reboot |
| `uart_baudrate` | 115200 | 1200..3000000 | reboot |
| `time_source_mode` | 0 | 0=local，1=external | reboot |
| `time_uart_baudrate` | 9600 | 1200..115200 | reboot |
| `pps_input_delay_us` | 0 | 0..1000 | reboot |
| `pps_period_us` | 1000000 | 固定 1 Hz | reboot |
| `pps_width_us` | 100000 | 10..500000 | reboot |
| `radio_delay_us` | 0 | 0..1000 | reboot |
| `status_interval_ms` | 1000 | 100..10000 | runtime |
| `led_heartbeat` | true | bool | runtime |
| `led_period_ms` | 1000 | 100..10000 | runtime |

CDC shell 示例：

```text
param list
param get role_id
param set role_id 1
param set group_id 1
param set group_key 00112233445566778899aabbccddeeff
param set time_source_mode 0
param clear time_source_mode
param reset
kernel reboot cold
```

当前 NVS ID 布局不兼容旧实验固件。项目不做旧 ID 迁移；升级旧板时直接执行相关
`param clear <name>` 或 `param reset`，然后按当前参数重新配置。

`group_id` 与 `group_key` 用于派生普通设备不易串组的 RF channel/address。它们不是消息认证
或加密协议；如果需要对抗恶意设备，必须另加认证与密钥管理。

## 构建

默认环境：

```text
NCS_WORKSPACE=/home/hv/ncs
ZEPHYR_BASE=/home/hv/ncs/zephyr
ZEPHYR_VENV=/home/hv/ncs/.venv
ZEPHYR_SDK_INSTALL_DIR=/home/hv/zephyr-sdk-0.17.4
BOARD=xiao_ble/nrf52840
```

`master`/`slave` 参数只决定默认输出目录；角色由同一镜像中的 NVS `role_id` 决定：

```bash
./build.sh master
./build.sh slave
```

validation 固件启用 CDC bridge 测试命令：

```bash
./build.sh master -d build/validation -- \
  -DOVERLAY_CONFIG=validation.conf
```

测试专用丢包镜像：

```bash
./build.sh master -d build/loss-validation -- \
  -DOVERLAY_CONFIG='validation.conf;loss-validation.conf'
```

`loss-validation.conf` 绝不能用于生产固件。

## 烧录与角色配置

可以手动双击 reset 进入 UF2 bootloader，也可以使用固定 USB ID 工具：

```bash
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py --list

/home/hv/ncs/.venv/bin/python tools/flash_uf2.py \
  --device-id DBE5C3D84EA2EC6F \
  --uf2 build/validation/zephyr/zephyr.uf2
```

烧录同一 UF2 后，分别通过 CDC shell 设置：

```text
# master
param set role_id 0
kernel reboot cold

# slave
param set role_id 1
kernel reboot cold
```

固件带 30 秒 watchdog 和持久化 boot guard。自动化工具在再次 cold flash 前先等待稳定启动
时间，并在普通 shell command timeout 时只重刷受影响的精确 USB ID 一次；第二次 timeout
直接失败并保存 raw logs 与 JSON summary，不会无限恢复。

## 双板端到端验证

```bash
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage local-check \
  --master-id DBE5C3D84EA2EC6F \
  --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600
```

runner 只接受完整 16-hex USB ID，不回退到 `/dev/ttyACM0`/`1`。每次运行在
`build/board-e2e/<timestamp>-<stage>/` 保存：

- `master.raw.log`；
- `slave.raw.log`；
- `summary.json`，包含固件 SHA-256、USB ID、双向结果、flash/retry/recovery 次数、
  queue drops、错误与起止时间。

validation CDC 命令包括 `bridge_test inject`、`verify`、`stats`、丢包注入和时间源状态测试；
生产固件不编译这些 payload 注入命令。

## 主机测试

```bash
ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/bridge_logic/zephyr/zephyr.exe

ZEPHYR_BASE=/home/hv/ncs/zephyr \
/home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
  tests/sync_logic -d build/tests/sync_logic
./build/tests/sync_logic/sync_logic/zephyr/zephyr.exe

/home/hv/ncs/.venv/bin/python -m unittest -v \
  tests/test_board_e2e.py tests/test_flash_uf2.py

for check in tests/*_static_check.sh; do bash "$check"; done
```

## 主要模块

| 文件 | 职责 |
| --- | --- |
| `src/timebase.c` | 1 MHz TIMER3 和硬件捕获/compare 通道 |
| `src/pps_output.c` / `src/pps_input.c` | PPS 输出和外部 PPS 捕获 |
| `src/nmea_parser.c` / `src/time_uart.c` | NMEA RMC/ZDA 与外部时间 UART |
| `src/utc_clock.c` | LOCAL/ACQUIRING/LOCKED/HOLDOVER 状态机 |
| `src/link_protocol.c` | ESB wire protocol 与 UTC publication |
| `src/link_scheduler_core.c` | 广播下行、轮询上行、membership 和 record 调度 |
| `src/record_queue.c` | 每 slave 完整记录队列 |
| `src/radio_transport.c` | ESB profile/transaction 与硬件事件 |
| `src/uart_bridge.c` | 物理 UART async RX 和 record-aware TX |
| `src/bridge_runtime.c` | 时间、无线、UART 和参数集成 |
| `src/param_config.c` / `src/param_shell.c` | 默认参数、NVS 与 CDC shell |
| `tools/board_e2e.py` | 固定 ID 双板自动验证、恢复和证据保存 |

## 未完成的外部验收

- 使用真实 GNSS NMEA + PPS 驯服 external master；
- 用示波器确认两板 PPS 都为 1 Hz、100 ms 高电平；
- 在 external locked 状态下测量 master/slave 上升沿相位误差并确认 `<=20 us`；
- 多于一块物理 slave 的并发压力测试。三 slave 的独立 record/round-robin 当前由 native
  test 覆盖，尚未由三块真实 slave 同时验证。
