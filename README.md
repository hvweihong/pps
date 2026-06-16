# XIAO nRF52840 Plus 无线 PPS 时间同步

这是一个运行在 Seeed XIAO nRF52840 Plus 上的 Zephyr 应用，用于实现多个
板卡之间的无线时间同步，并输出 1 Hz PPS 信号。

当前方案使用一块板作为 master，一块或多块板作为 slave。master 以本地硬件
timer 作为时间基准；slave 接收 raw nRF RADIO 同步包，估计本地 timer 与
master timer 之间的 offset，并把自己的 PPS 输出相位对齐到 master 的 PPS
epoch。

本实现不使用 BLE。无线链路直接使用 nRF52840 RADIO 外设；时间戳捕获和 PPS
边沿输出使用 TIMER、PPI/GPPI、GPIOTE 硬件链路，避免 Zephyr 调度延迟影响
关键时序。

## 当前状态

- 开发板：`xiao_ble/nrf52840`
- 目标硬件：Seeed XIAO nRF52840 Plus
- master PPS 基准：master 本地 timer
- master 外部 PPS 输入：暂未实现
- 无线传输：nRF RADIO proprietary mode，1 Mbit/s
- PPS 输出：D1 / P0.03，高电平有效
- PPS 周期：1 秒
- PPS 脉宽：100 us
- heartbeat LED：开启，每 1 秒翻转一次
- 当前实测固定偏差补偿：slave `CONFIG_TIME_SYNC_RADIO_DELAY_US=17`

在当前硬件 PPS 输出路径下，已经观察到的主要残余误差是固定时延偏差。
`CONFIG_TIME_SYNC_RADIO_DELAY_US` 对 PPS 相位大约是 1:1 调整；改变
`CONFIG_TIME_SYNC_INTERVAL_US` 在当前测试环境下没有明显改变 jitter。

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

系统内部使用一个统一的抽象时间戳：本地 32-bit 硬件 TIMER 的微秒计数，并在
软件中扩展为 64 bit。

master 和 slave 的 timer 并不物理共享。slave 通过无线观测计算 offset：

```text
offset = master_radio_reference_tick - slave_radio_reference_tick
slave_pps_tick = master_next_pps_tick - offset
```

关键点是：无线参考时间戳由硬件捕获，而不是在整包接收完成后的中断里读取。

## 无线同步包链路

master 周期性地把一次 RADIO 发送调度到未来某个 TIMER tick：

```text
TIMER3 compare -> PPI/GPPI -> RADIO TXEN
```

同步包中包含：

- magic/version/role 字段
- sequence number
- network ID
- `master_tx_tick`：master 估算的 RADIO ADDRESS 参考时间
- `next_pps_master_tick`：该无线参考时间之后的下一次 master PPS 上升沿
- sync interval
- 软件 CRC32

RADIO 硬件本身也启用了硬件 packet CRC。

slave 侧直接把 RADIO ADDRESS 事件捕获到 TIMER3：

```text
RADIO ADDRESS event -> PPI/GPPI -> TIMER3 CAPTURE
```

RADIO END 中断只负责解包、校验和更新状态；它不定义接收时间戳。

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

板级差异、radio 参考点差异、GPIO 路径、测量通道 skew 等剩余固定偏差，通过
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

TIMER2 配置为 1 MHz：

```text
1 timer tick = 1 us
```

当 slave 计算出新的 PPS 相位后，会通过共享 timebase 调度一次 phase reset：

```text
TIMER3 compare -> PPI/GPPI -> TIMER2 CLEAR/START + GPIOTE SET
```

因此 PPS 边沿不是由 Zephyr thread 或 sleep 翻转 GPIO 产生。Zephyr 仍负责
初始化、日志、无线包解码和控制逻辑。

## 软件模块

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | master/slave 主控制循环和数据流 |
| `src/timebase.c` | 1 MHz TIMER3 timebase 和 compare 调度 |
| `src/radio_sync.c` | raw nRF RADIO 初始化、TX 调度、RX 时间戳捕获 |
| `src/pps_output.c` | 硬件 PPS 生成和 phase reset |
| `src/sync_filter.c` | offset/drift 估计和锁定状态 |
| `src/sync_packet.c` | 同步包 encode/decode 和软件 CRC |
| `src/status.c` | 周期性状态日志 |
| `src/heartbeat_led.c` | 1 秒 heartbeat LED |

## 配置项

通用配置在 `Kconfig` 中。

关键配置：

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
| `CONFIG_TIME_SYNC_STATUS_INTERVAL_MS` | 状态日志间隔 | `1000` |
| `CONFIG_TIME_SYNC_LED_HEARTBEAT_PERIOD_MS` | heartbeat LED 周期 | `1000` |

master 和 slave 必须使用相同的 `NETWORK_ID`、`RF_CHANNEL` 和
`TIME_SYNC_INTERVAL_US`。

## 编译

辅助脚本默认使用之前安装的 Zephyr workspace：

```bash
./build.sh master
./build.sh slave
```

默认输出：

```text
build/master/zephyr/zephyr.uf2
build/slave/zephyr/zephyr.uf2
```

脚本默认路径：

```text
ZEPHYR_WORKSPACE=/home/hv/zephyrproject
ZEPHYR_BASE=/home/hv/zephyrproject/zephyr
ZEPHYR_VENV=/home/hv/zephyrproject/.venv312
ZEPHYR_SDK_INSTALL_DIR=/home/hv/zephyr-sdk-1.0.1
BOARD=xiao_ble/nrf52840
```

如果本机路径不同，可以用环境变量覆盖：

```bash
ZEPHYR_WORKSPACE=/path/to/zephyrproject ./build.sh slave
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
role=master seq=<n> state=locked offset=0us ... pps=<count> tx=<count>
```

slave 日志示例：

```text
role=slave seq=<n> state=<state> offset=<us> drift=<ppm> missed=<n> rx=<count>
```

slave 预期锁定流程：

```text
unlocked -> acquiring -> locked
```

如果 `rx` 持续增加但 PPS 没有对齐，优先检查
`CONFIG_TIME_SYNC_RADIO_DELAY_US` 和示波器测得的 PPS 相位。如果 `rx` 不增加，
检查 role、RF channel、network ID 和天线距离。

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
- timebase 分辨率是 1 us。
- 校准项是整数微秒。
- filter 使用整数 offset 修正，不适合期待亚微秒级收敛。
- 同一时间只使用一个 RADIO channel/network 配置。
- 当前没有配对流程。只要 `NETWORK_ID` 和 `RF_CHANNEL` 一致，板子就会加入同一
  同步网络。

## 后续优化方向

- 增加 master 外部 PPS 输入捕获。
- 把 timebase 提升到 16 MHz，或使用 fixed-point tick，支持亚微秒校准。
- 把 radio delay 和 filter offset 改成 fixed-point 或 ns 单位。
- 在 `master_to_local()` 中真正应用 drift 补偿，而不是只记录/打印 drift。
- 增加 debug GPIO，用于观测 RADIO ADDRESS 和 PPS phase reset 事件。
- 增加 per-board calibration 的持久化存储。
