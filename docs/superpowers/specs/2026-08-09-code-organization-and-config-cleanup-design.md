# Code Organization, Configuration, and Test Cleanup Design

## Goal

在不改变已验证的无线时间同步、1 Hz PPS、UART bridge、分组隔离和自动恢复行为的前提下，
删除无效历史实现，按功能整理源码和测试，收敛运行时参数，并精简没有实际价值的 Kconfig
选项；同时提供只包含“快速开始”和“工程介绍”两部分的 README。

## Scope and constraints

- 当前分支和当前工作区继续使用，不创建 worktree。
- 同一份固件仍通过 NVS `role_id` 配置为 master/slave；不恢复旧 NVS ID 布局兼容。
- master 下行保持 best-effort 广播；slave 上行保持每 slave 独立记录和可靠轮询。
- 外部时间源仍支持 master 本地授时和外部 NMEA/PPS 驯服两种 NVS 模式。
- 每个代码阶段必须有主机回归和至少一次双板 600-byte 双向硬件门禁；最终阶段追加物理 UART smoke。
- 历史 Git 提交、阶段证据和历史设计文档保留，不把历史记录当作当前构建输入。

## Architecture

源码按功能移动到以下目录，先保持现有公共接口和运行时调用关系，再做小范围死分支清理：

```text
src/
├── app/          # main、watchdog/boot guard、USB DFU、状态和 LED 集成
├── config/       # 参数定义、NVS backend、参数 shell、编译默认值
├── bridge/       # UART bridge、record queue、wire protocol、membership、scheduler
├── radio/        # ESB transport、地址派生和 radio-facing glue
├── time/         # timebase、PPS、NMEA、UTC、无线时间同步数学/状态
└── validation/   # validation CDC 注入/校验和 loss test hooks
```

本轮不重写 `bridge_runtime.c` 或 `link_scheduler_core.c` 的协议算法；若移动过程中发现
明确未调用的静态函数或已失效分支，先以测试保护，再单独删除并验证。

## Dead-code removal

下列代码已确认不在现行 production/validation 构建路径中，删除前用 `rg` 和构建 source
list 复核：

- `src/ble_time_sync*.c/.h`：旧 BLE 时间同步栈；
- `src/radio_sync.c/.h`：旧 MPSL timeslot radio sync；
- `src/sync_packet.c/.h`：只服务旧 `radio_sync` 的包模型及其专用 native 测试；
- `src/runtime_config.c/.h`：只被旧 BLE service 使用的运行时配置；
- 与上述实现绑定的 retired BLE/MPSL static checks。

所有其他删除必须满足：无现行 CMake source、无现行 include/callers、无设计型测试依赖，
并在提交说明中列出路径和依据。

## Runtime parameters and Kconfig audit

### NVS parameters retained

NVS 只保存现场部署会调整的参数：

`role_id`、`group_id`、`group_key`、`uart_baudrate`、`time_source_mode`、
`time_uart_baudrate`、`pps_input_delay_us`、`radio_delay_us`。

这些参数仍遵循“有效 NVS 值优先，否则使用编译默认值”的规则；旧布局不迁移，升级时由
`param reset` 清除。

### NVS parameters removed

ring size、聚合超时、无线调度周期/slot/window/lease、最大 slave 数、固定 PPS 周期、状态
日志周期和 LED 心跳不再进入 NVS。它们是编译期资源、协议时序或诊断策略，不应被现场配置
改变；其中仍有调优价值的项目可保留为 Kconfig 编译选项。

### Kconfig audit rules

阶段三逐项建立“符号 → C/CMake 消费者 → 资源/时序影响 → 是否可变”的清单：

- 删除没有代码消费者或仅用于历史分支的 `RADIO_BRIDGE_NEW_STACK`、
  `RADIO_BRIDGE_VALIDATION_STAGE` 等符号；
- 删除值域被固定为单值且没有变体价值的符号，例如固定 1 Hz 周期和固定最大 slave 数；
- 保留会影响 RAM/flash、ESB 时序、硬件驱动或不同产品构建的编译期选项，例如 ring size、
  聚合/slot 调度参数、PPS 脉宽和 validation/loss overlay 开关；
- 生产默认配置不启用 validation/loss；测试开关只在对应 overlay 中出现，不进入 README
  的现场参数表；
- 对保留的用户默认值统一命名和注释，避免同一参数同时有 Kconfig、C 宏和 NVS 三份不一致
  的默认定义。

## Tests

保留有明确设计目的的测试并按边界重组：

```text
tests/
├── unit/bridge/       # protocol、ring、record、membership、scheduler policy
├── unit/time/         # sync filter、UTC、NMEA、time math
├── unit/time_uart/    # async RX/restart/error handling
└── host/              # board_e2e/flash tool contracts and recovery evidence
```

删除源码文本匹配型 `*_static_check.sh`；其关键约束分别由 Kconfig/CMake fail-closed 校验、
native unit tests、production/validation 编译和板级门禁覆盖。主机测试保留 exact-ID、恢复
预算、readiness freshness、drop/error 解析和 summary 完整性等安全回归，不保留只验证字符串
存在的过程测试。

## README

README 只保留两个一级部分：

1. **快速开始**：依赖、production/validation 编译、UF2 烧录、引脚接线、角色/分组/时间源
   参数、双板 gate 和常用 CDC shell 命令；
2. **工程介绍**：拓扑、PPS/无线时间同步、UART bridge 数据语义、参数优先级、分组隔离、
   源码目录职责、固件变体边界和外部 GNSS/PPS 验收边界。

历史验证矩阵和逐阶段证据继续放在 `docs/verification/`，不复制到 README。

## Verification and commits

每个阶段完成以下检查后单独提交：

1. native unit suites；
2. host unit suites；
3. production/validation firmware build；
4. 一次双板 600-byte 双向 gate，检查 `LOCKED` 和零 drop/error；
5. `ruff`、`git diff --check`。

最终阶段额外执行物理 UART 双向 smoke 和现有多尺寸/多频率/延时矩阵的回归抽样，确认整理
没有改变已验证行为。

## Non-goals

- 不重新设计 ESB 协议或可靠性语义；
- 不新增 BLE 功能；
- 不声称未接入 GNSS/示波器时的 PPS 电气相位精度；
- 不在本轮拆分并重写整个 scheduler/runtime 算法。
