# XIAO nRF52840 Plus 无线 PPS 与 UART Bridge

## 快速开始

### 环境与构建

工程面向 Seeed XIAO nRF52840 Plus，默认使用 nRF Connect SDK v3.3.1、
Zephyr SDK 0.17.4 和板目标 `xiao_ble/nrf52840`。构建脚本默认在
`/home/hv/ncs` 查找 NCS 与 Python 虚拟环境；其他安装位置可通过
`NCS_WORKSPACE`、`ZEPHYR_BASE`、`ZEPHYR_VENV` 和
`ZEPHYR_SDK_INSTALL_DIR` 覆盖。

同一份镜像通过 NVS 配置为 master 或 slave，构建阶段不区分角色。

```bash
./build.sh -d build/production
./build.sh -d build/validation -- -DOVERLAY_CONFIG=validation.conf
```

- `build/production/zephyr/zephyr.uf2`：生产固件，物理 UART bridge、无线时间同步、
  PPS、参数 shell 和运行日志。
- `build/validation/zephyr/zephyr.uf2`：在生产功能之上增加 CDC payload/time
  注入与校验命令，用于没有外部串口或 GNSS 时的双板自动化测试。
- `loss-validation.conf` 仅用于丢包恢复测试，禁止用于生产部署。

### 接线

所有电平为 3.3 V，外部设备必须与板子共地。UART 使用 8N1、无硬件流控，TX/RX
需要交叉连接。

| 用途 | 板端 TX | 板端 RX/输入 | 默认配置 | 说明 |
| --- | --- | --- | --- | --- |
| 数据 UART bridge (`uart0`) | P1.11 | P1.12 | 921600 baud | master/slave 业务数据 |
| 外部时间 UART (`uart1`) | P0.04 | P0.05 | 9600 baud | external master 接收 NMEA RMC/ZDA |
| PPS 输出 | P0.03 | - | 1 Hz，100 ms 高电平 | 所有角色输出 |
| PPS 输入 | - | P0.02 | 上升沿捕获 | 仅 external master 使用 |
| USB CDC | USB | USB | 115200 line coding | shell、日志；validation 测试通道 |

```text
board TX  -> adapter RX
board RX  <- adapter TX
board GND -- adapter GND
```

### 烧录与运行时配置

双击 reset 可进入 UF2 bootloader。自动化烧录必须使用完整 16 位十六进制 USB ID，避免多板
环境中刷错设备。

```bash
/home/hv/ncs/.venv/bin/python tools/flash_uf2.py --list

/home/hv/ncs/.venv/bin/python tools/flash_uf2.py \
  --device-id DBE5C3D84EA2EC6F \
  --uf2 build/validation/zephyr/zephyr.uf2
```

CDC shell 中只有以下 8 个持久化参数；全部在 cold reboot 后生效。

| 参数 | 编译默认值 | 有效范围/格式 | 用途 |
| --- | ---: | --- | --- |
| `role_id` | 0 | 0=master，1..3=slave | 同一镜像选择角色 |
| `group_id` | 1 | 1..0xFFFFFFFE | 网络分组 |
| `group_key` | `00112233445566778899aabbccddeeff` | 32 个十六进制字符 | 派生 RF 地址 |
| `uart_baudrate` | 921600 | 1200..3000000 | 数据 UART |
| `time_source_mode` | 0 | 0=local，1=external | master 时间源 |
| `time_uart_baudrate` | 9600 | 1200..115200 | NMEA UART |
| `pps_input_delay_us` | 0 | 0..1000 | 外部 PPS 输入延迟补偿 |
| `radio_delay_us` | 0 | 0..1000 | slave 无线延迟校准 |

有效 NVS 值优先；参数未持久化、key 被清除、存储 backend 不可用或加载失败时，使用
Kconfig/C 代码中的编译默认值。当前 NVS ID 布局不兼容旧实验固件；升级旧板前先在旧
固件上执行 `param reset`，再烧录和重新配置。

```text
# 查看
param list
param get role_id

# master：本地时间对外授时
param set role_id 0
param set group_id 1
param set group_key 00112233445566778899aabbccddeeff
param set time_source_mode 0
kernel reboot cold

# master：外部 NMEA + PPS 驯服
param set role_id 0
param set time_source_mode 1
param set time_uart_baudrate 9600
param set pps_input_delay_us 0
kernel reboot cold

# slave；group_id/group_key 必须与 master 一致
param set role_id 1
param set group_id 1
param set group_key 00112233445566778899aabbccddeeff
kernel reboot cold
```

修改单个参数可用 `param clear <name>` 恢复编译默认值；`param reset` 清除全部 8 项。

### 双板端到端验证

以下命令会按固定 USB ID 刷写两块板，自动配置角色，并验证无线锁定、CDC 模拟 UART
bridge 双向数据、队列/驱动错误计数与证据落盘。

```bash
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage local-check \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600
```

每次运行在 `build/board-e2e/<timestamp>-<stage>/` 保存两板原始日志和
`summary.json`。普通 shell command timeout 最多只自动重刷受影响的精确 USB ID 一次；
再次超时立即失败，不会无限恢复。

开发回归包括 5 个 native suite 和 host runner 测试：

```bash
for suite in bridge config radio time time_uart; do
  ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build \
    -p always -b native_sim/native "tests/unit/${suite}" -d "build/tests/${suite}"
  "./build/tests/${suite}/${suite}/zephyr/zephyr.exe"
done

/home/hv/ncs/.venv/bin/python -m unittest discover -v -s tests/host -t .
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
```

## 工程介绍

### 功能与数据拓扑

```text
                         best-effort broadcast
                    +----------------------------+
                    |                            v
host UART <-> master board                 slave board <-> host UART
                    ^                            |
                    +------ polled uplink -------+
                           per-slave records
```

- master 下行向同组 slave 广播，不保存 repair history，也不等待业务 ACK；丢失一帧不阻塞
  后续数据。
- slave 上行由 master 轮询并确认；未确认记录会重发。master 为每个 slave 保留独立完整
  record queue，再按轮询顺序输出到 UART，避免不同 slave 的字节流被合并。
- slave 之间没有直接业务收发路径。协议支持最多 3 个活动 slave；当前板级回归使用一块
  master 和一块 slave，多 slave 调度由 native 测试覆盖。
- 生产路径使用物理 UART。validation 固件把相同 bridge 边界扩展到 CDC，以便电脑未连接
  外部串口时仍能验证完整无线链路。

### 无线时间同步与 PPS

所有板使用 1 MHz TIMER3 作为统一 64-bit 微秒 timebase。PPS 输出由 TIMER compare、
GPIOTE 与 GPPI/PPI 在硬件侧产生：P0.03 固定输出 1 Hz、默认 100 ms 高电平脉冲，
上升沿代表本地的整秒边界。

master 有两种 NVS 可选模式：

- `time_source_mode=0`：使用本地 timebase 持续对外授时；没有外部 UTC，因此 UTC quality
  标记为无效，但 PPS 与无线相位基准继续工作。
- `time_source_mode=1`：组合 P0.02 外部 PPS 上升沿与 UART1 NMEA RMC/ZDA。连续有效配对后
  进入 `EXTERNAL_LOCKED`；输入缺失时进入 `HOLDOVER`，以最后基准和本地 timebase 继续
  输出。

slave 从连续无线同步帧估计 master/slave offset，校正本地 PPS epoch，并通过
`time_sync status` 报告 `ACQUIRING/LOCKED` 状态。双板自动化已经验证无线 `LOCKED`、
1 Hz scheduler 持续运行以及冷重启后的重新锁定。

### 参数、分组与安全边界

参数层只持久化现场部署需要调整的 8 项。ring 容量、广播/轮询时序、PPS 脉宽、日志与 LED
等资源或诊断策略属于 Kconfig 编译期配置，避免同一设置同时存在多套运行时来源。

`group_id` 决定 RF channel，并与 `group_key` 共同派生 RF address；不同普通设备组已在双板测试中验证
不会互相发现或锁定。它们用于避免普通设备误串组，不提供消息认证、保密或对抗恶意设备
的安全边界；需要安全通信时必须另加认证、加密和密钥生命周期管理。

### 代码结构

| 路径 | 职责 |
| --- | --- |
| `src/app/` | 启动、watchdog/boot guard、状态日志、LED |
| `src/config/` | 编译默认值、8 项 NVS 参数、CDC shell |
| `src/bridge/` | wire protocol、ring/record、membership、scheduler、UART bridge 集成 |
| `src/radio/` | RF 地址派生、AES 辅助与 ESB transport |
| `src/time/` | timebase、PPS、无线同步、NMEA、UTC 状态机 |
| `src/validation/` | validation-only CDC bridge/time 注入与校验 |
| `tests/unit/` | bridge/config/radio/time/time-UART 设计型 native 测试 |
| `tests/host/` | 固定 ID 烧录、自动恢复、证据与 runner 测试 |
| `tools/` | UF2 烧录与双板 E2E 工具 |

### 验证边界

production 固件不编译 CDC payload/time 注入或丢包注入；validation overlay 只增加可控测试
入口，仍运行相同的物理 UART、scheduler、radio 和时间同步核心。完整阶段证据记录在
`docs/verification/incremental-board-e2e-results.md`。

当前双板验证已覆盖同一镜像角色配置、NVS 默认/持久化优先级、group 隔离、无线锁定、
双向 CDC 与物理 UART 数据、冷重启和自动恢复。以下项目仍需真实外部仪器完成最终验收：

- 用真实 GNSS NMEA + PPS 驯服 external master，并验证丢失输入后的 holdover；
- 用示波器确认 master/slave PPS 都为 1 Hz、100 ms 高电平；
- 在 external locked 状态测量两板 PPS 上升沿相位误差并确认不超过 20 us；
- 使用三块真实 slave 做并发吞吐与独立 record 压力测试。
