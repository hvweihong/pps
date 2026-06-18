# XIAO nRF52840 Plus 无线 PPS 时间同步

这是一个运行在 Seeed XIAO nRF52840 Plus 上的 Zephyr/NCS 应用，用于在多块
板卡之间做无线时间同步，并输出 1 Hz PPS 信号。

当前实现使用一块板作为 master，一块或多块板作为 slave。master 以本地
TIMER3 时间为基准；slave 接收 master 的 proprietary RADIO 同步包，估计本地
TIMER3 与 master TIMER3 之间的 offset，然后把自己的 PPS 输出相位对齐到
master 的 PPS epoch。

本版本已迁移到 Nordic MPSL timeslot 框架：应用不再直接长期占用 RADIO。
所有 proprietary RADIO 收发都只在 MPSL 授权的 timeslot callback 内配置和
执行。BLE 控制链路也已启用，由 MPSL 在 BLE 与私有无线同步之间做无线资源
调度。

## 当前状态

- 开发板：`xiao_ble/nrf52840`
- 目标硬件：Seeed XIAO nRF52840 Plus
- SDK：nRF Connect SDK，默认本机路径 `/home/hv/ncs`
- Zephyr SDK：默认本机路径 `/home/hv/zephyr-sdk-0.17.4`
- 无线传输：nRF RADIO proprietary mode，1 Mbit/s
- RADIO 访问方式：MPSL timeslot
- master 时间基准：master 本地 TIMER3
- master 外部 PPS 输入：暂未实现
- PPS 输出：D1 / P0.03，高电平有效
- PPS 周期：1 秒
- PPS 脉宽：100 us
- heartbeat LED：开启，每 1 秒翻转一次
- 当前实测固定偏差补偿：slave `CONFIG_TIME_SYNC_RADIO_DELAY_US=17`
- BLE 配组方式：方案 B，master 为 Central，slave 为 Peripheral
- 启动顺序：先做 proprietary RADIO 同步，RADIO 稳定后再启动 BLE
- slave RX 方式：未锁定时全周期 acquire 窗口，锁定后预测短窗口

## 硬件连接

PPS 测量引脚如下：

| 信号 | XIAO 引脚 | nRF52840 GPIO | 方向 | 说明 |
| --- | --- | --- | --- | --- |
| PPS 输出 | D1 | P0.03 | 输出 | 1 Hz 高电平脉冲 |
| 预留 PPS 输入 | D0 | P0.02 | 输入/预留 | 当前未使用 |
| Debug 输出 | 板级相关 | P0.28 | 输出/预留 | overlay 中已定义，当前应用未使用 |

PPS 输出由 `boards/xiao_ble_nrf52840.overlay` 中的 `pps-out` devicetree alias
定义。

## 时间同步原理

系统内部使用 TIMER3 作为统一 timebase：

```text
TIMER3 frequency = 1 MHz
1 tick = 1 us
```

TIMER3 是 32-bit 硬件 timer，软件扩展成 64-bit 微秒时间戳。master 和 slave
各自运行独立 TIMER3，slave 通过无线观测估计：

```text
offset = master_radio_reference_tick - slave_radio_reference_tick
slave_pps_tick = master_next_pps_tick - offset
```

关键点是：无线参考时间戳不是在整包接收完成后的线程或中断中读取，而是在
MPSL timeslot 内用 RADIO ADDRESS 事件通过 PPI/GPPI 捕获到 `MPSL_TIMER0`。
timeslot 开始时会把 `MPSL_TIMER0` 配成 1 MHz，并记录它和全局 TIMER3
timebase 的对应关系；收到 ADDRESS 后再把捕获值换算回全局 TIMER3 微秒时间。

## MPSL Timeslot 数据流

MPSL 负责调度 RADIO/TIMER0/AAR/CCM 这些无线协议共享资源。当前代码只在
timeslot callback 里访问 RADIO 和 MPSL_TIMER0。

启动阶段优先保证 proprietary RADIO 同步。默认
master `CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US=25000`，slave
`CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US=100000`，slave acquire 窗口覆盖完整
100 ms 同步周期。窗口会在 timeslot 开始后留出
`CONFIG_TIME_SYNC_RX_START_OFFSET_US` 的硬件准备时间再触发 RXEN。此时 BLE controller 还未 `bt_enable()`，不广播、不扫描，
所以 slave 的长 acquisition timeslot 不会和 BLE 空口事件竞争。这样 slave 在
第一次锁定前更容易收到 master beacon；master TX timeslot 仍保持短窗口，BLE
启动后也有调度空间。

同步稳定后才启动 BLE：

- master 连续成功发送若干个 RADIO beacon 后，才启动 BLE Central 扫描；
- slave 的同步滤波器进入 locked，并连续收到若干个 locked beacon 后，才启动
  BLE Peripheral 广播；
- slave 进入 locked 后，RX 从全周期 acquire 窗口切换成预测短窗口，把无线资源
  让给 BLE connection event。

Kconfig 默认 `CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START=n`，便于单独做 RADIO-only
验证。当前 `master.conf` 已设置 `CONFIG_TIME_SYNC_MASTER_BLE_AUTO_START=y`，
用于在 RADIO TX 稳定后自动启动 master BLE 扫描并验证 BLE 配组/hello 链路。

因此如果日志里看到 BLE 连接后出现 `radio tx failed: -62`，应优先检查 BLE 是否
过早启动、扫描/广播占空比是否过高，以及 locked 短窗口是否已经生效。

slave 启动 RX：

```text
slave loop 计算下一次 RX window
  -> radio_sync_schedule_rx_window()
  -> 无 timeslot anchor 时用 EARLIEST 建立 anchor
  -> 有 anchor 后用 NORMAL distance 对齐预测 window_start
  -> MPSL_TIMESLOT_SIGNAL_START
  -> MPSL_TIMER0 配置为 1 MHz，并记录 TIMER0/TIMER3 对齐参考点
  -> 按 window_start 在 MPSL_TIMER0 内触发 RADIO RXEN
  -> RADIO ADDRESS -> GPPI/PPI -> MPSL_TIMER0 CAPTURE
  -> RADIO END -> 把 TIMER0 capture 换算为 TIMER3 timebase
  -> 解包、校验、提交观测值
  -> MPSL_TIMER0 compare 到 window_end
  -> 停 RADIO，结束本次 window
```

master 发送同步包：

```text
main loop 调用 radio_sync_start_master() 一次
  -> mpsl_timeslot_request(EARLIEST) 建立首次授权窗口
  -> MPSL_TIMESLOT_SIGNAL_START
  -> MPSL_TIMER0 配置为 1 MHz，并记录 TIMER0/TIMER3 对齐参考点
  -> 配置 proprietary RADIO TX
  -> 按 CONFIG_TIME_SYNC_MASTER_TX_START_OFFSET_US 计算 TXEN compare
  -> MPSL_TIMER0 compare -> GPPI/PPI -> RADIO TXEN
  -> RADIO ADDRESS -> GPPI/PPI -> MPSL_TIMER0 CAPTURE
  -> RADIO END -> 停 RADIO
  -> callback 返回 MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST
  -> 下一次 NORMAL timeslot distance = CONFIG_TIME_SYNC_INTERVAL_US
```

master 的发包边沿由 timeslot 内的 `MPSL_TIMER0` compare 硬件触发 RADIO
TXEN。MPSL callback 只负责在授权窗口内完成 RADIO/PPI/TIMER0 配置，不用线程
睡眠或中断回调直接翻转关键时序。

同步包里的 `master_tx_tick` 是 master 根据当前 timeslot 参考点预测的 RADIO
ADDRESS 时间，并用实际 ADDRESS capture 统计 `tx_err`：

```text
master_tx_tick = txen_tick + CONFIG_TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US
```

master 同时捕获实际 ADDRESS 时间并打印 `tx_err`/`tx_ref` 作为诊断；slave
捕获本地 ADDRESS 时间并打印 `rx_ref`。这些字段用于确认 timeslot 内时间戳映射
是否稳定。

## 无线同步包

同步包中包含：

- magic/version/role
- sequence number
- network ID
- `master_tx_tick`：master 估算的 RADIO ADDRESS 参考时间
- `next_pps_master_tick`：该无线参考时间之后的下一次 master PPS 上升沿
- sync interval
- 软件 CRC32

RADIO 硬件自身也启用了 packet CRC。slave 只有在 network ID、软件 CRC 和
硬件 CRC 都通过后才更新同步滤波器。

## RADIO ADDRESS 参考点

当前 RADIO 配置：

- 模式：`NRF_RADIO_MODE_NRF_1MBIT`
- 频率：`2400 MHz + CONFIG_TIME_SYNC_RF_CHANNEL`
- 默认 channel：40，即 2440 MHz
- preamble：8 bit
- base address length：4 bytes
- prefix：1 byte
- whitening：开启
- fast ramp-up：开启

master 当前使用以下估算值：

```text
TXEN -> ADDRESS = 88 us
```

计算来源：

```text
fast ramp-up TXEN->READY: 40 us
1 Mbit/s preamble:         8 us
1 Mbit/s 5-byte address:   40 us
total:                     88 us
```

对应配置项：

```text
CONFIG_TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US=88
```

板级差异、radio 参考点差异、GPIO 路径、示波器通道 skew 等剩余固定偏差，通过
slave 侧参数校准：

```text
CONFIG_TIME_SYNC_RADIO_DELAY_US
```

当前测试环境中，`17 us` 可以补偿已观测到的固定 PPS 偏差。

## PPS 输出链路

PPS 由硬件生成。

稳定的 1 Hz PPS 波形由 TIMER2 和 GPIOTE 产生：

```text
TIMER2 COMPARE0 -> PPI/GPPI -> GPIOTE SET   -> PPS 上升沿
TIMER2 COMPARE1 -> PPI/GPPI -> GPIOTE CLEAR -> PPS 下降沿
```

当 slave 计算出新的 PPS 相位后，会通过共享 timebase 调度一次 phase reset：

```text
TIMER3 compare -> PPI/GPPI -> TIMER2 CLEAR/START + GPIOTE SET
```

因此 PPS 边沿不是由 Zephyr thread、sleep 或 GPIO API 翻转产生。Zephyr 负责
初始化、日志、无线包解码和控制逻辑；关键 PPS 边沿和无线时间戳仍走硬件链路。

## BLE 配组与控制链路

当前实现使用方案 B：

```text
master: BLE Central + proprietary RADIO TX + PPS 输出
slave:  BLE Peripheral + proprietary RADIO RX + PPS 输出
```

slave 启动后不会立刻广播。它先用 proprietary RADIO acquire master beacon，
同步滤波器 locked 并稳定后，才广播自定义 time-sync GATT service UUID，并使用
设备名 `TS-SLAVE`。master 启动后也不会立刻扫描；它先连续成功发送一段 RADIO
beacon，确认 master TX 稳定后，再启动低占空比 passive scan。发现带该 service
UUID 或 `TS-SLAVE` 名称的设备后建立 BLE 连接，发现 RX command characteristic，
然后写入：

```text
SET_GROUP network=0x54534e52 channel=40 interval=100000
START_SYNC
HELLO_FROM_MASTER hello world
```

连接成功并完成上述写入的 slave 进入该 master 的授时组。若 slave 已经按相同
`NETWORK_ID/RF_CHANNEL/INTERVAL` 锁定，`SET_GROUP` 和 `START_SYNC` 会被当作
幂等命令处理，不会重置已经稳定的同步。`HELLO_FROM_MASTER hello world` 用于
验证普通 BLE 数据互发，slave 收到后会 notify `HELLO_FROM_SLAVE hello world`。
BLE 连接只负责配组、状态和后续普通数据通信；PPS 精度仍由 proprietary RADIO
的 ADDRESS 硬件时间戳和硬件 PPS 输出链路决定，BLE 包不参与授时时间戳。

slave 暴露的自定义 UART-like GATT service 包含：

| Characteristic | 方向 | 属性 | 用途 |
| --- | --- | --- | --- |
| RX command | Central -> Peripheral | Write / Write Without Response | 接收文本命令 |
| TX notify | Peripheral -> Central | Notify | 返回 ACK/状态 |
| STATUS | Central read | Read | 读取当前状态 |

支持的文本命令：

| 命令 | 作用 |
| --- | --- |
| `GET_STATUS` | 返回当前 BLE 与授时组状态 |
| `SET_GROUP network=0x... channel=... interval=...` | 设置运行时授时组参数 |
| `START_SYNC` | 按当前授时组重启 RADIO RX 并进入 acquire |
| `STOP_SYNC` | 停止 proprietary RADIO RX，PPS 保持当前/free-run |

第一版不做 BLE bonding，也不把配置写入 flash；断电重启后仍回到 Kconfig 默认
授时组。一个房间内有多个 master 时，不同 master 可以使用不同
`NETWORK_ID/RF_CHANNEL`，slave 被哪个 master 通过 BLE 连接并写入配置，就加入
哪个授时组。

## Windowed RX

slave 在启动/未锁定阶段使用完整同步周期 RX window，锁定后才切换成短预测
window。

Acquire 模式用于刚启动、刚收到 `START_SYNC`、未锁定或连续丢包后的搜索：

```text
CONFIG_TIME_SYNC_RX_ACQUIRE_WINDOW_US=100000
CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US=2000
CONFIG_TIME_SYNC_RX_START_OFFSET_US=1000
```

acquire 阶段 BLE 尚未启动，因此优先用全周期 RX 捕获 master beacon。
`CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US` 在当前实现中表示两个 acquire window
之间的重试间隔，而不是完整 RX 周期。默认 2 ms gap 可以避免 100 ms RX window
和 100 ms master beacon 长期固定错相；进入 locked 后，slave 再根据滤波器预测
下一个 beacon 时间，只打开几毫秒的短窗口。实际 acquire RX 长度会扣除
timeslot 结束清理余量，因此默认约为 98 ms 级别，不会顶到 MPSL timeslot 尾部。
如果 BLE 已经启动后又连续丢失 locked/recovery 窗口，slave 会回到 acquire，但
此时 acquire 使用 recovery 级别的较宽短窗口做相位扫描，避免 BLE 连接期间再次
申请接近 100 ms 的长 timeslot。

Locked 模式用于已经锁定 master beacon 节奏后的预测接收：

```text
next_master_tx = last_beacon.master_tx_tick + sync_interval_us
predicted_local_rx = sync_filter_master_to_local(next_master_tx)
window_start = predicted_local_rx - CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US
window_len = pre_margin + post_margin + guard
```

默认窗口参数：

```text
CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US=1500
CONFIG_TIME_SYNC_RX_LOCKED_POST_MARGIN_US=2500
CONFIG_TIME_SYNC_RX_RECOVERY_PRE_MARGIN_US=10000
CONFIG_TIME_SYNC_RX_RECOVERY_POST_MARGIN_US=10000
CONFIG_TIME_SYNC_RX_WINDOW_GUARD_US=300
CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE=3
```

如果 locked 短窗口错过 beacon，slave 会先进入 `recovery_window`，围绕下一次
预测 beacon 打开更宽的恢复窗口；连续错过
`CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE` 次后会清空旧 filter/offset 并退回
acquire。这样可以覆盖短时 timeslot 冲突、master 短暂抖动、slave 预测误差和
clock holdover 误差。若收到的包相位跳变超过滤波器阈值，slave 会丢弃旧 offset
并用新包重新开始 acquire，以支持 master 重启或 slave 长时间失锁后的恢复。状态日志中的
`rx_mode/rx_win/rx_open/rx_skip/rx_late` 用于观察当前 RX 窗口状态。

## 软件模块

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | master/slave 主控制循环和数据流 |
| `src/timebase.c` | 1 MHz TIMER3 timebase 和 compare 调度 |
| `src/radio_sync.c` | MPSL timeslot、proprietary RADIO TX/RX、硬件时间戳 |
| `src/pps_output.c` | 硬件 PPS 生成和 phase reset |
| `src/sync_filter.c` | offset/drift 估计和锁定状态 |
| `src/sync_packet.c` | 同步包 encode/decode 和软件 CRC |
| `src/status.c` | 周期性状态日志 |
| `src/heartbeat_led.c` | 1 秒 heartbeat LED |
| `src/runtime_config.c` | 运行时授时组配置 |
| `src/ble_time_sync.c` | BLE 初始化、连接状态和命令队列 |
| `src/ble_time_sync_service.c` | slave 自定义 GATT service |
| `src/ble_time_sync_client.c` | master BLE Central 扫描、连接和配组写入 |

## 配置项

通用配置在 `Kconfig` 中。

| Kconfig 配置 | 含义 | 当前/默认值 |
| --- | --- | --- |
| `CONFIG_TIME_SYNC_ROLE_MASTER` | 编译为 master | 由 `master.conf` 设置 |
| `CONFIG_TIME_SYNC_ROLE_SLAVE` | 编译为 slave | 由 `slave.conf` 设置 |
| `CONFIG_TIME_SYNC_NETWORK_ID` | 逻辑同步网络 ID | `0x54534e52` |
| `CONFIG_TIME_SYNC_RF_CHANNEL` | RADIO 频率，基于 2400 MHz 的 offset | `40` |
| `CONFIG_TIME_SYNC_INTERVAL_US` | master 同步包间隔 | `100000` |
| `CONFIG_TIME_SYNC_PPS_PERIOD_US` | PPS 周期 | `1000000` |
| `CONFIG_TIME_SYNC_PPS_WIDTH_US` | PPS 高电平宽度 | `100` |
| `CONFIG_TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US` | master TXEN 到 ADDRESS 的估算时间 | `88` |
| `CONFIG_TIME_SYNC_RADIO_DELAY_US` | slave 残余相位校准 | 当前 slave 值：`17` |
| `CONFIG_TIME_SYNC_MASTER_TX_TIMESLOT_LENGTH_US` | master 周期 TX 使用的短 timeslot 长度 | `3000` |
| `CONFIG_TIME_SYNC_MASTER_TX_START_OFFSET_US` | master 在 timeslot 开始后多久触发 TXEN | `500` |
| `CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US` | MPSL timeslot 长度 | master `25000`，slave `100000` |
| `CONFIG_TIME_SYNC_TIMESLOT_END_MARGIN_US` | timeslot 结束前停止 RADIO 的余量 | `250` |
| `CONFIG_TIME_SYNC_RX_START_OFFSET_US` | slave 在 timeslot 开始后触发 RXEN 的偏移 | `1000` |
| `CONFIG_TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US` | earliest timeslot 请求等待上限 | `200000` |
| `CONFIG_TIME_SYNC_BLE` | 启用 BLE 控制链路 | `y` |
| `CONFIG_TIME_SYNC_BLE_DEVICE_NAME_PREFIX` | BLE 名称前缀 | `TS` |
| `CONFIG_TIME_SYNC_RX_ACQUIRE_WINDOW_US` | acquire RX 窗口长度 | `100000` |
| `CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US` | acquire RX 窗口间隔 | `2000` |
| `CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US` | locked 预测点前置余量 | `1500` |
| `CONFIG_TIME_SYNC_RX_LOCKED_POST_MARGIN_US` | locked 预测点后置余量 | `2500` |
| `CONFIG_TIME_SYNC_RX_RECOVERY_PRE_MARGIN_US` | recovery 预测点前置余量 | `10000` |
| `CONFIG_TIME_SYNC_RX_RECOVERY_POST_MARGIN_US` | recovery 预测点后置余量 | `10000` |
| `CONFIG_TIME_SYNC_RX_WINDOW_GUARD_US` | RX 窗口保护余量 | `300` |
| `CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE` | 连续丢包后退回 acquire 的阈值 | `3` |
| `CONFIG_TIME_SYNC_STATUS_INTERVAL_MS` | 状态日志间隔 | `1000` |
| `CONFIG_TIME_SYNC_LED_HEARTBEAT_PERIOD_MS` | heartbeat LED 周期 | `1000` |

master 会通过 BLE 把自己的 `NETWORK_ID`、`RF_CHANNEL` 和
`TIME_SYNC_INTERVAL_US` 写给 slave。手动调试时，这三个参数仍必须一致。

## 编译环境

当前应用依赖 MPSL，所以需要 nRF Connect SDK，而不是单独 Zephyr workspace。

正常安装方式：

```bash
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.1 /home/hv/ncs
cd /home/hv/ncs
west update
```

脚本默认路径：

```text
NCS_WORKSPACE=/home/hv/ncs
ZEPHYR_WORKSPACE=/home/hv/ncs
ZEPHYR_BASE=/home/hv/ncs/zephyr
ZEPHYR_VENV=/home/hv/ncs/.venv
ZEPHYR_SDK_INSTALL_DIR=/home/hv/zephyr-sdk-0.17.4
BOARD=xiao_ble/nrf52840
```

如果本机路径不同，可以用环境变量覆盖：

```bash
NCS_WORKSPACE=/path/to/ncs ZEPHYR_SDK_INSTALL_DIR=/path/to/sdk ./build.sh slave
```

## 编译

编译 master：

```bash
./build.sh master
```

编译 slave：

```bash
./build.sh slave
```

默认输出：

```text
build/master/zephyr/zephyr.uf2
build/slave/zephyr/zephyr.uf2
```

需要指定 build 目录时：

```bash
./build.sh slave -d build/slave-cal
```

需要透传额外 `west build` 参数时，放在 `--` 后面：

```bash
./build.sh slave -- --verbose
```

如果要修改 PPS 校准等 Kconfig 配置，建议编辑 `master.conf`、`slave.conf` 或
`Kconfig`，然后重新编译。

## 烧录

双击 XIAO nRF52840 Plus 的 reset 按钮进入 UF2 bootloader。Windows 会出现一个
可移动磁盘。把生成的 UF2 文件拷贝进去即可：

```text
master 板：build/master/zephyr/zephyr.uf2
slave 板： build/slave/zephyr/zephyr.uf2
```

UF2 文件不需要重命名。

## 运行日志

应用默认每秒输出一次状态日志。

master 日志示例：

```text
role=master seq=<n> state=locked offset=0us ... pps=<count> tx=<count> tx_err=<us> tx_ref=<us> ble_conn=<0|1>
```

slave 日志示例：

```text
role=slave seq=<n> state=<state> offset=<us> drift=<ppm> missed=<n> rx=<count> rx_mode=<mode> rx_win=<us> ble_conn=<0|1>
```

slave 预期锁定流程：

```text
unlocked -> acquiring -> locked
```

新增 MPSL 相关计数：

| 字段 | 含义 |
| --- | --- |
| `ts_block` | MPSL timeslot 请求被更高优先级无线事件阻塞 |
| `ts_cancel` | 已排队 timeslot 被取消 |
| `ts_over` | 应用超出 timeslot 使用时间，出现该值需要立即排查 |
| `tx_busy` | master 线程侧发现 TX 状态机未 idle |
| `tx_req` | master 调用 `mpsl_timeslot_request()` 直接失败 |
| `tx_block` | master TX timeslot 被 MPSL blocked |
| `tx_cancel` | master TX timeslot 被 MPSL cancelled |
| `tx_to` | master RADIO TX 未等到 END 的超时次数 |
| `tx_time` | master TX 时序准备失败次数 |
| `tx_late` | master 进入 timeslot 后已来不及安排 TXEN 的次数 |
| `tx_seq` | master 周期 TX 状态机发送的序号 |
| `tx_err` | master 实际 ADDRESS 捕获时间与计划 ADDRESS 时间的差值 |
| `tx_ref` | master ADDRESS 事件距 timeslot 参考点的时间 |
| `rx_ref` | slave ADDRESS 事件距 timeslot 参考点的时间 |
| `rx_mode` | `stopped`、`acquire`、`recovery_window` 或 `locked_window` |
| `rx_win` | 当前 RX 窗口长度 |
| `rx_open` | 已打开 RX window 次数 |
| `rx_skip` | RX window 被 MPSL blocked/cancelled 或跳过次数 |
| `rx_late` | 主循环发现窗口已经太晚而放弃的次数 |
| `ble_conn` | 当前 BLE 连接状态 |
| `ble_scan_seen` | master 扫描回调收到的 BLE 广播/扫描响应次数 |
| `ble_scan_match` | master 扫描到符合 UUID 或名称过滤条件的 slave 次数 |
| `ble_scan_type_drop` | master 因广播类型不可连接而丢弃的扫描事件 |
| `ble_scan_filter_drop` | master 因 UUID/名称不匹配而丢弃的扫描事件 |
| `ble_conn_req` | master 发起 BLE 连接的次数 |
| `ble_conn_fail` | master 连接创建或连接完成失败次数 |
| `pps_sched` | 当前计划的下一次 PPS 上升沿 |
| `pps_last` | 最近一次 PPS 上升沿 |
| `pending` | 是否正在等待一次 PPS phase reset 生效 |

如果 `rx` 持续增加但 PPS 没有对齐，优先检查
`CONFIG_TIME_SYNC_RADIO_DELAY_US` 和示波器测得的 PPS 相位。如果 `rx` 不增加，
检查 role、RF channel、network ID、天线距离，以及 `ts_block/ts_cancel` 是否
持续增长。

## PPS 校准

用示波器同时测量 master 和 slave 的 PPS 输出。

建议统一使用以下表达：

```text
master = slave + delta
```

如果测量结果是：

```text
master = slave -16.6 us
```

表示 slave 比 master 晚约 16.6 us。此时应把 slave 的
`CONFIG_TIME_SYNC_RADIO_DELAY_US` 增加约 16 到 17 us。

当前测试环境中：

```conf
CONFIG_TIME_SYNC_RADIO_DELAY_US=17
```

可以把平均 PPS 相位补偿到接近 0。

由于当前校准单位是整数微秒，静态校准粒度约为 1 us。可以在以下两个值中选择
长期平均误差更小的一个：

```text
16 us: 预期约 -0.6 us
17 us: 预期约 +0.4 us
```

如果改变 `CONFIG_TIME_SYNC_INTERVAL_US` 后 jitter 没有明显变化，说明当前主要
剩余误差是固定路径/参考点偏差，而不是 timer holdover 漂移。

## 已知限制

- master 暂未捕获或驯服到外部 PPS 输入。
- BLE 第一版不做 bonding，也不做配置持久化。
- master 第一版自动连接看到的 slave，并下发当前编译配置；还没有白名单或人工
  选择界面。
- timebase 分辨率是 1 us。
- 校准项是整数微秒。
- filter 使用整数 offset 修正，不适合期待亚微秒级收敛。
- slave 运行时只有一个 active 授时组；再次收到 `SET_GROUP/START_SYNC` 会切换
  到新的授时组。

## 后续优化方向

- 为 BLE 配组增加白名单、bonding 或手机/串口选择界面。
- 增加 master 外部 PPS 输入捕获。
- 把 locked RX window 从 earliest request 进一步收敛到更严格的 normal/absolute
  调度。
- 把 timebase 提升到 16 MHz，或使用 fixed-point tick，支持亚微秒校准。
- 把 radio delay 和 filter offset 改成 fixed-point 或 ns 单位。
- 在 `master_to_local()` 中真正应用 drift 补偿，而不是只记录/打印 drift。
- 增加 debug GPIO，用于观测 RADIO ADDRESS、timeslot start/end 和 PPS reset。
- 增加 per-board calibration 的持久化存储。
