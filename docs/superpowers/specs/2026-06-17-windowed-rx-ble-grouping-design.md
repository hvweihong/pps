# Windowed RX and BLE Grouping Design

## 背景

当前 XIAO nRF52840 Plus 应用已经把 proprietary RADIO 同步链路迁移到 Nordic
MPSL timeslot，并通过硬件 ADDRESS capture 与硬件 PPS 输出达到约 1-2 us 的
master/slave PPS 实测误差。

当前 slave RX 仍然使用接近连续的 100 ms timeslot。这对无 BLE 场景有利，但
会挤占 Bluetooth Controller 的连接事件，不适合作为 BLE 并发长期方案。

## 目标

1. slave 只在特定预测窗口内打开 proprietary RADIO RX。
2. 同时启用 BLE 通信。
3. 使用方案 B 做 BLE 授时组配对：
   - master 作为 BLE Central。
   - slave 作为 BLE Peripheral。
   - master 连接 slave 后，通过 GATT 写入授时组配置。
   - 连接成功并配置完成的 slave 加入该 master 的授时组。
4. 保持 PPS 关键链路不依赖 BLE。BLE 只用于配置、状态和普通数据通信。

## 非目标

- 第一版不实现 BLE bonding/pairing 安全绑定。
- 第一版不实现配置持久化到 flash。
- 第一版不实现多个 master 之间的协调或冲突检测。
- 第一版不把 BLE 包作为授时时间戳来源。
- 第一版不追求最小 RX 窗口；优先保证不丢包和不破坏现有 1-2 us PPS 精度。

## 系统角色

### Master

master 保持现有 proprietary RADIO TX 和 PPS 输出：

- 使用本地 TIMER3 作为时间基准。
- 周期性通过 MPSL timeslot 发送 sync beacon。
- 使用硬件 PPS 输出 1 Hz。

新增 BLE Central 行为：

- 扫描带有 time-sync service UUID 的 slave 广播。
- 连接 slave。
- 发现 slave 的 UART-like GATT service。
- 写入授时组配置：`network_id`、`rf_channel`、`sync_interval_us`。
- 接收 slave 状态 notify。

### Slave

slave 保持现有 proprietary RADIO RX、sync filter 和 PPS 输出：

- 未锁定时使用较宽 acquire RX 窗口搜索 master。
- 锁定后根据下一次 beacon 的预测到达时间开短 RX 窗口。
- 收到 sync beacon 后继续使用 RADIO ADDRESS capture 更新滤波器。

新增 BLE Peripheral 行为：

- 广播 time-sync service UUID 和设备名。
- 暴露 UART-like GATT service。
- 接收 master 写入的授时组配置。
- 通过 notify 上报 role、lock state、network ID、RF channel、PPS 计数和
  timeslot 统计。

## BLE 服务设计

第一版实现自定义 UART-like GATT service，不依赖 Nordic UART Service 库，以便
命令格式和状态字段完全受控。

Service UUID 使用项目自定义 128-bit UUID。包含三个 characteristic：

1. RX command characteristic
   - 属性：Write Without Response + Write。
   - 方向：Central -> Peripheral。
   - 用途：master 向 slave 写命令。

2. TX notify characteristic
   - 属性：Notify。
   - 方向：Peripheral -> Central。
   - 用途：slave 向 master 返回 ACK、状态和事件。

3. STATUS characteristic
   - 属性：Read。
   - 方向：Central 读取。
   - 用途：无需订阅 notify 时读取当前状态快照。

命令使用 ASCII 文本，便于用 nRF Connect App 或串口式工具调试：

```text
GET_STATUS
SET_GROUP network=0x54534e52 channel=40 interval=100000
START_SYNC
STOP_SYNC
```

第一版命令语义：

- `GET_STATUS`：返回当前状态。
- `SET_GROUP ...`：更新 slave 的运行时授时组参数。
- `START_SYNC`：按当前授时组参数启动或重启 proprietary RADIO RX。
- `STOP_SYNC`：停止 proprietary RADIO RX，PPS 保持 free-run。

第一版 `SET_GROUP` 成功后会立即切换运行时配置，但不写 flash。重新上电后仍使用
Kconfig 默认值。

## 授时组配对流程

```text
slave boot
  -> BLE advertise "TS-SLAVE-xxxx"
  -> 等待 master 连接

master boot
  -> BLE scan
  -> 找到 time-sync service UUID 的 slave
  -> connect
  -> discover GATT service
  -> write SET_GROUP network=<master network> channel=<master channel> interval=<master interval>
  -> write START_SYNC

slave 收到 START_SYNC
  -> 停止当前 RX timeslot 请求
  -> 应用新的 network/channel/interval
  -> 进入 acquire RX 窗口模式
  -> 收到 master sync beacon
  -> locked 后进入预测 RX 窗口模式
```

BLE 连接关系只定义“这个 slave 被哪个 master 配置”。真正的授时组仍由
proprietary RADIO 参数决定：

- `network_id`
- `rf_channel`
- `sync_interval_us`

## Windowed RX 设计

### Acquire 模式

用途：slave 未锁定、刚被 BLE 配组、连续丢包后重新搜索。

默认参数：

```text
CONFIG_TIME_SYNC_RX_ACQUIRE_WINDOW_US=12000
CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US=100000
```

行为：

- 每 100 ms 申请一个约 12 ms timeslot。
- 在 timeslot 内打开 RADIO RX。
- 收到有效 sync beacon 后更新 sync filter。
- 达到 lock 条件后切换到 locked 模式。

### Locked 模式

用途：slave 已经知道 master beacon 节奏，只围绕预计到达时间开 RX。

默认参数：

```text
CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US=1500
CONFIG_TIME_SYNC_RX_LOCKED_POST_MARGIN_US=2500
CONFIG_TIME_SYNC_RX_WINDOW_GUARD_US=300
CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE=3
```

预测：

```text
next_master_tx = last_beacon.master_tx_tick + sync_interval_us
predicted_local_rx = sync_filter_master_to_local(next_master_tx)
window_start = predicted_local_rx - pre_margin
window_len = pre_margin + post_margin + guard
```

如果当前实现的 `sync_filter_master_to_local()` 仍只使用 offset，不使用 drift
预测，第一版保持 4 ms 级窗口，避免晶振漂移、MPSL normal request jitter 和
BLE 连接事件导致漏包。

连续错过 `CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE` 个 beacon 后，slave 退回
acquire 模式。

## MPSL Timeslot 策略

- proprietary RADIO 仍只在 MPSL timeslot callback 内配置和使用。
- timeslot 中继续使用 `RADIO ADDRESS -> PPI/GPPI -> MPSL_TIMER0 CAPTURE`
  作为无线时间戳。
- RX timeslot 不再连续 request 100 ms normal slot。
- locked 模式使用短 normal timeslot，并尽量把 priority 设置为 normal。
- master TX timeslot 可继续使用 high priority，因为它决定所有 slave 的同步
  参考包是否按时发出。
- 如果 locked RX timeslot 被 BLE 或更高优先级无线事件 blocked/cancelled，
  记录计数并跳过该次 beacon；连续丢失后退回 acquire。

## 配置项

新增 Kconfig：

```text
CONFIG_TIME_SYNC_BLE=y
CONFIG_TIME_SYNC_BLE_DEVICE_NAME_PREFIX
CONFIG_TIME_SYNC_RX_ACQUIRE_WINDOW_US=12000
CONFIG_TIME_SYNC_RX_ACQUIRE_PERIOD_US=100000
CONFIG_TIME_SYNC_RX_LOCKED_PRE_MARGIN_US=1500
CONFIG_TIME_SYNC_RX_LOCKED_POST_MARGIN_US=2500
CONFIG_TIME_SYNC_RX_WINDOW_GUARD_US=300
CONFIG_TIME_SYNC_RX_MISSED_TO_ACQUIRE=3
```

运行时配置结构新增：

```c
struct time_sync_runtime_config {
	uint32_t network_id;
	uint8_t rf_channel;
	uint32_t sync_interval_us;
};
```

该结构由 Kconfig 默认值初始化，可被 BLE `SET_GROUP` 在运行时更新。

## 状态与诊断

新增或扩展日志字段：

- `ble_conn`：BLE 是否连接。
- `ble_peer`：连接 peer 地址短格式。
- `rx_mode`：`acquire` 或 `locked_window`。
- `rx_win`：当前 RX 窗口长度。
- `rx_open`：RX 窗口开启次数。
- `rx_skip`：因窗口过晚或状态不一致跳过次数。
- `rx_block` / `rx_cancel`：MPSL blocked/cancelled 计数。

保留现有字段：

- `tx_err`
- `tx_ref`
- `rx_ref`
- `pps_sched`
- `pps_last`
- `pps_epoch`
- `pending`

## 验证标准

1. 无 BLE 连接时：
   - `./build.sh master` 和 `./build.sh slave` 通过。
   - master/slave PPS 仍能锁定。
   - 示波器 PPS 误差保持在当前 1-2 us 级别。

2. BLE 配组时：
   - master 能扫描并连接 slave。
   - master 能写 `SET_GROUP` 和 `START_SYNC`。
   - slave notify 返回 ACK 和状态。
   - slave 加入 master 的 proprietary RADIO 授时组。

3. BLE 连接保持时：
   - slave locked 后 `rx_mode=locked_window`。
   - `rx_win` 约为 4 ms 级别，而不是 100 ms。
   - BLE 连接不因同步 timeslot 长期断开。
   - PPS 误差无明显退化。

## 风险与缓解

- 风险：locked RX 窗口过窄导致漏包。
  - 缓解：第一版使用 4 ms 默认窗口；连续漏包退回 acquire。

- 风险：BLE 连接事件与 sync RX 窗口冲突。
  - 缓解：RX 使用短 timeslot，记录 blocked/cancelled；必要时增大 BLE 连接间隔。

- 风险：运行时切换 network/channel 时 RADIO 状态混乱。
  - 缓解：切换前停止 RX session 状态，清空 sync filter，再重新 acquire。

- 风险：GATT 文本命令解析复杂。
  - 缓解：第一版只支持固定命令和固定 key，输入长度限制在一个 ATT 写入包内。
