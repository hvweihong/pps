# Default Data UART 921600 Baud Design

## Goal

将物理数据 UART bridge 的编译默认波特率从 115200 改为 921600，并在双板上验证无线时间
同步、CDC bridge 和 921600 baud 物理 UART 双向链路仍正常。

## Scope

- `RADIO_BRIDGE_UART_BAUDRATE` 的 Kconfig 默认值改为 921600。
- `uart_baudrate` NVS 参数仍允许 `1200..3000000`，有效持久化值仍优先于编译默认值。
- 两块现有测试板清除已持久化的 `uart_baudrate` 后 cold reboot，以验证实际采用
  `921600 (default)`。
- README 的数据 UART 引脚表和参数表更新为 921600。

以下行为不变：

- USB CDC shell/validation 通道的 line coding；
- 外部时间 UART 的 9600 默认值和 `1200..115200` 范围；
- master/slave 角色、无线协议、PPS、分组和恢复逻辑；
- 数据 UART 的 8N1、无硬件流控配置。

## Implementation

先把 config native test 中编译默认值及断言改为 921600，并在旧 Kconfig 默认值下观察预期
失败；随后只修改 Kconfig 默认值，使测试转绿。其他测试中用于验证 NVS set/get 的 115200
样例可以保留，因为它仍是合法的用户配置值。

README 在既有“两部分”结构内同步更新，不新增章节。Kconfig 审计文档只描述该符号为何保留，
不需要因默认值变化修改决策。

## Verification

1. config native test 完成 RED/GREEN，并运行全部五套 native、host 和 ruff 回归。
2. 构建 production 与 validation 固件。
3. 在两板 validation 固件上执行 `param clear uart_baudrate` 和 cold reboot，要求两板均报告
   `uart_baudrate = 921600 (default, reboot)`。
4. 运行双板无线/CDC 最终门禁，要求 slave `LOCKED`、双向 payload 精确、零重试/恢复/drop/error。
5. 烧录 production 固件，将 `/dev/ttyACM2` 和 adb `/dev/ttyS1` 配置为 921600 8N1，至少抽测
   32-byte/10 pps 与 64-byte/100 pps 的双向独立帧，校验 CRC/SHA 并记录延时
   min/P50/P95/P99/max。

若任一物理串口端无法稳定使用 921600，保留失败证据并分析硬件/驱动限制，不静默回退到
115200。
