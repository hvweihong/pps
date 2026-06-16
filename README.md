# XIAO nRF52840 Plus 无线 PPS 时间同步

这是一个运行在 Seeed XIAO nRF52840 Plus 上的 Zephyr/NCS 应用，用于在多块
板卡之间做无线时间同步，并输出 1 Hz PPS 信号。

当前实现使用一块板作为 master，一块或多块板作为 slave。master 以本地
TIMER3 时间为基准；slave 接收 master 的 proprietary RADIO 同步包，估计本地
TIMER3 与 master TIMER3 之间的 offset，然后把自己的 PPS 输出相位对齐到
master 的 PPS epoch。

本版本已迁移到 Nordic MPSL timeslot 框架：应用不再直接长期占用 RADIO。
所有 proprietary RADIO 收发都只在 MPSL 授权的 timeslot callback 内配置和
执行。这样后续可以继续接入 Bluetooth Controller/BLE，由 MPSL 在 BLE 与
私有无线同步之间做无线资源调度。

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

BLE 目前还没有启用；本次迁移只是把 proprietary RADIO 访问放进 MPSL
timeslot，为后续 BLE 并发做准备。

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

slave 启动 RX：

```text
radio_sync_start_rx()
  -> mpsl_timeslot_request(EARLIEST)
  -> MPSL_TIMESLOT_SIGNAL_START
  -> MPSL_TIMER0 配置为 1 MHz，并记录 TIMER0/TIMER3 对齐参考点
  -> 配置 proprietary RADIO RX
  -> RADIO ADDRESS -> GPPI/PPI -> MPSL_TIMER0 CAPTURE
  -> RADIO END -> 把 TIMER0 capture 换算为 TIMER3 timebase
  -> 解包、校验、提交观测值
  -> MPSL_TIMER0 compare near slot end
  -> 停 RADIO，REQUEST 下一次 NORMAL timeslot
```

master 发送同步包：

```text
main loop 选择未来 txen_tick
  -> radio_sync_send_beacon_at()
  -> mpsl_timeslot_request(EARLIEST)
  -> MPSL_TIMESLOT_SIGNAL_START
  -> MPSL_TIMER0 配置为 1 MHz，并记录 TIMER0/TIMER3 对齐参考点
  -> 配置 proprietary RADIO TX
  -> 根据 txen_tick 计算 timeslot 内 MPSL_TIMER0 compare
  -> MPSL_TIMER0 compare -> GPPI/PPI -> RADIO TXEN
  -> RADIO ADDRESS -> GPPI/PPI -> MPSL_TIMER0 CAPTURE
  -> RADIO END -> 停 RADIO，结束 timeslot
```

master 的发包边沿由 timeslot 内的 `MPSL_TIMER0` compare 硬件触发 RADIO
TXEN。MPSL callback 只负责在授权窗口内完成 RADIO/PPI/TIMER0 配置，不用线程
睡眠或中断回调直接翻转关键时序。

同步包里的 `master_tx_tick` 是 master 计划的 RADIO ADDRESS 时间：

```text
master_tx_tick = planned_txen_tick + CONFIG_TIME_SYNC_RADIO_TXEN_TO_ADDRESS_US
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
| `CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US` | MPSL timeslot 长度 | `100000` |
| `CONFIG_TIME_SYNC_TIMESLOT_END_MARGIN_US` | timeslot 结束前停止 RADIO 的余量 | `250` |
| `CONFIG_TIME_SYNC_TIMESLOT_REQUEST_TIMEOUT_US` | earliest timeslot 请求等待上限 | `200000` |
| `CONFIG_TIME_SYNC_STATUS_INTERVAL_MS` | 状态日志间隔 | `1000` |
| `CONFIG_TIME_SYNC_LED_HEARTBEAT_PERIOD_MS` | heartbeat LED 周期 | `1000` |

master 和 slave 必须使用相同的 `NETWORK_ID`、`RF_CHANNEL` 和
`TIME_SYNC_INTERVAL_US`。

当前 `CONFIG_TIME_SYNC_TIMESLOT_LENGTH_US=100000` 是 MPSL 单个 timeslot 的最大
长度，用于尽量接近迁移前的连续 RX 行为。后续启用 BLE 后，应把 slave RX
timeslot 缩小到同步包预计到达窗口附近，减少对 BLE 连接事件的挤占。

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
role=master seq=<n> state=locked offset=0us ... pps=<count> tx=<count> tx_err=<us> tx_ref=<us> ts_block=<n>
```

slave 日志示例：

```text
role=slave seq=<n> state=<state> offset=<us> drift=<ppm> missed=<n> rx=<count> rx_ref=<us> ts_cancel=<n>
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
| `tx_err` | master 实际 ADDRESS 捕获时间与计划 ADDRESS 时间的差值 |
| `tx_ref` | master ADDRESS 事件距 timeslot 参考点的时间 |
| `rx_ref` | slave ADDRESS 事件距 timeslot 参考点的时间 |
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
- BLE 还没有启用；当前只是把 proprietary RADIO 放进 MPSL timeslot。
- slave RX 当前使用接近连续的 100 ms timeslot 窗口；BLE 启用后需要缩窄窗口。
- timebase 分辨率是 1 us。
- 校准项是整数微秒。
- filter 使用整数 offset 修正，不适合期待亚微秒级收敛。
- 同一固件实例只使用一个 RADIO channel/network 配置。
- 当前没有配对流程。只要 `NETWORK_ID` 和 `RF_CHANNEL` 一致，板子就会加入同一
  同步网络。

## 后续优化方向

- 启用 BLE，并用 BLE 连接关系派生授时组配置。
- 增加 master 外部 PPS 输入捕获。
- 缩小 slave RX timeslot，只围绕预计同步包窗口开 RADIO。
- 把 timebase 提升到 16 MHz，或使用 fixed-point tick，支持亚微秒校准。
- 把 radio delay 和 filter offset 改成 fixed-point 或 ns 单位。
- 在 `master_to_local()` 中真正应用 drift 补偿，而不是只记录/打印 drift。
- 增加 debug GPIO，用于观测 RADIO ADDRESS、timeslot start/end 和 PPS reset。
- 增加 per-board calibration 的持久化存储。
