# 渐进式双板端到端验证设计

日期：2026-08-08
验证分支：`validation/incremental-board-e2e`
基线：`main` (`61b5cae`)
固定设备：

- `DBE5C3D84EA2EC6F`：master
- `5B3D71D27A709CA2`：slave

## 目标

从已知的 `main` BLE/MPSL 无线授时实现出发，逐层迁移星型 ESB UART bridge 功能。每次只引入一个可归因的功能层，并在两块实板上完成软件、启动和双板运行验证，直到两块板同时稳定运行完整 UART bridge 与无线时间同步固件。

最终必须证明：

1. 两块板都能稳定启动并持续保持 USB CDC 枚举。
2. master/slave 能通过 ESB 完成发现、分配、入网、轮询和重新入网。
3. slave 能接收无线时间同步帧、进入锁定状态，并在重启后重新锁定。
4. 两块板的 UART bridge 驱动都成功初始化，异步 RX 保持使能，TX 能完成，空载无 restart/drop 错误。
5. 不依赖物理 UART 适配器，通过验证专用 CDC 命令向 bridge 数据边界注入和读取确定性数据，验证 master→slave 与 slave→master 的无线数据路径。
6. NVS 参数、运行时角色和 group 配置在重启后保持生效。

## 本轮不声称完成的项目

- D6/D7 物理 UART 引脚上的外部设备收发正确性，因为当前没有 USB-UART 适配器或物理环回连接。
- PPS 引脚的相位误差和脉宽精度，因为当前没有示波器或逻辑分析仪。
- 三个 slave 同时在线和第四候选等待，因为当前只有一个 slave。

`uhubctl` 已安装在 `/usr/sbin/uhubctl`。当前外置 hub 报告 ganged power switching，不能安全地只断一个板；如果能控制整个 hub，则把两块板同时断电作为“联合真实冷启动”验收，不把它用于单板故障归因。其他缺少外部硬件的项目必须在结果文档中明确标记为“未测”。

## 仓库与分支策略

不使用 worktree。所有工作均在 `/home/hv/projects/pps` 当前工作区进行。

开始本设计前，原 `feature/star-radio-uart-bridge-main` 的本地改动已经提交为 `6fb76cd`。当前工作区已直接从 `main@61b5cae` 创建 `validation/incremental-board-e2e`。CodeGraph 数据库和 Python 字节码只通过 `.git/info/exclude` 本地忽略，不删除、不提交。

## 总体方法

每个阶段只启用一个新的功能边界。所有阶段使用相同顺序：

1. 添加或迁移对应的自动测试。
2. 先运行测试并确认它因功能尚未实现而失败。
3. 迁移满足该阶段要求的最小实现。
4. 运行目标测试、完整 native 测试和静态检查。
5. pristine 构建目标固件并记录 SHA、配置和资源占用。
6. 固定 USB 序列号，一次只操作一块板。
7. 完成单板启动循环，再运行双板功能验收。
8. 把命令、日志、计数、失败样本和结论追加到验证结果文档。

任何阶段失败时立即停止，不继续启用下一层。先复现、定位失败边界、提出单一假设并做最小验证；确认根因后才写回归测试和修复。

## 验证阶段

### 阶段 0：主线软件基线

在未改变固件行为前运行 `main` 已有 native 测试、静态检查和 master/slave pristine 构建。记录现有通过项、已知失败、固件尺寸和关键 Kconfig。此阶段用于区分原有问题与后续迁移引入的问题。

### 阶段 1：可恢复启动基线

从 feature 分支迁移串号绑定 UF2 工具及其单元测试，向 `main` 最小迁移硬件 watchdog 和 1200-baud bootloader 触发。1200-baud 回调必须独立于 ESB bridge runtime，使 BLE/MPSL 基线也能自动进入 UF2 bootloader。

两块板分别使用原 `main` master/slave 固件。每块板先完成 5 次 UF2 bootloader→application 冒烟循环，再完成 30 次 `kernel reboot cold` 循环。若能控制整个 hub，则另做 10 次 hub off→on 联合冷启动循环。要求每次都按原 USB 序列号重新枚举、出现完整启动 banner、watchdog 不触发且 shell 可响应。

### 阶段 2：BLE/MPSL 无线同步与 PPS 对照

保持 `main` 原无线架构不变，运行双板无线授时。要求 master 持续发送，slave 从 acquire 进入 locked，计数持续增长且未出现持续 MPSL/RADIO 错误。PPS 只验证启动成功、计数增长、slave 重启后重新锁定和重新调度，不声称引脚相位精度。

该阶段证明共享的 USB、timebase、PPS 和 `main` 无线基线在两块板上稳定。

### 阶段 3：NVS 参数基础

迁移参数定义、校验、NVS backend 和 CDC `param` shell，但暂不让参数改变无线后端。验证默认值、范围拒绝、set/get/clear、全量恢复默认值和软冷复位后的持久化。

此阶段单独隔离 flash settings 初始化和写入，避免它与 UART/ESB 同时引入。

### 阶段 4：UART bridge 硬件初始化

迁移 byte ring 和异步 UART bridge，暂不接入无线调度器。两块板都必须满足：

- `uart_configure()`、`uart_callback_set()` 和首次 `uart_rx_enable()` 成功。
- RX 不发生持续 disabled/restart error。
- 验证专用 TX 命令能提交有限长度数据并收到 TX 完成事件。
- 空载运行期间 RX/TX drop 为零。
- UART 功能关闭时 BLE/MPSL 对照仍稳定，打开后也不破坏 USB、授时或 PPS。

验证命令仅在验证 Kconfig 打开时编译，不成为正式协议接口。

### 阶段 5：固定角色 ESB 传输

关闭 BLE/MPSL radio backend，启用 Nordic ESB，但暂不启用发现、membership、动态 profile 切换和业务调度。master 固定 PTX，slave 固定 PRX；不使用未经证实的 PTX warmup。

在 `esb_init()`、地址/频道配置和 `esb_start_rx()` 前后写入启动阶段标记。要求两块板分别完成 30 次冷复位循环；USB 必须重新枚举，watchdog 不得在 ESB 初始化阶段复位，固定 ping/ack 计数持续增长。

该阶段是定位原冷启动卡死是否来自首次 ESB/RADIO 初始化的关键边界。

### 阶段 6：发现与动态 profile

迁移地址派生、协议 codec、membership、发现响应窗口、ASSIGN 和动态 profile 切换。启用 HELLO→ASSIGN→active 的最小控制流程，暂不启用 UART 业务数据和无线 PPS 校时。

要求 slave 自动成为 active node 1；slave 重启后能重新发现和分配；master 或 slave 连续重启不导致另一块板 USB 消失。此阶段每块板完成 30 次冷复位，重点记录 `esb_disable()` 前后阶段标记、reset reason、ASSIGN TX 完成状态和 active/suspect 变化。

### 阶段 7：ESB 无线时间同步与 PPS

迁移无线同步帧、ADDRESS 捕获、同步滤波器和 slave PPS epoch 调整。要求：

- master 的 sync TX 计数持续增长。
- slave 的 sync RX 计数持续增长并进入 locked。
- missed 不持续超过 received，offset/jitter 有界且不发散。
- slave 重启后重新入网并重新锁定。
- 两块板 PPS 计数持续增长，无调度错误。

在没有示波器时，offset/jitter 和 PPS 计数只证明软件锁定及硬件调度链路在运行，不证明引脚相位误差。

### 阶段 8：完整 bridge runtime

迁移 scheduler、repair、DROP_OLDEST、运行时角色/group 和完整 bridge runtime。增加仅用于验证的 CDC bridge 命令，在 UART 与 scheduler 的边界注入和提取确定性字节序列：

- master 注入下行数据，slave 在写入物理 UART 前截取并校验。
- slave 注入上行数据，master 在写入物理 UART 前截取并校验。
- 使用超过单帧的数据验证分片、序列、重排和聚合。
- 使用可控丢包注入验证 retry/repair，最终 payload 必须一致。
- 空载和无丢包场景所有 drop/error 计数必须为零。

最终完整固件每块板完成 50 次冷复位循环。随后分别重启 master 和 slave，验证 USB 重新枚举、slave 重新 active、同步重新 locked、PPS 计数恢复增长以及双向验证 payload 再次通过。

## 启动诊断与故障证据

验证构建使用可关闭的启动阶段标记，至少覆盖：

1. watchdog 初始化完成。
2. timebase 初始化前后。
3. PPS 初始化前后。
4. settings/NVS 初始化前后。
5. UART bridge 初始化前后。
6. `esb_init()` 前后。
7. `esb_start_rx()` 前后。
8. `esb_disable()` 前后。
9. bridge thread 启动。

同时记录 nRF reset reason、当前 stage、固件 SHA、角色、group、启动序号和 watchdog reset。若 RADIO 内部死等导致 USB 消失，watchdog 复位后的首个 banner 必须报告上次阶段，以便在没有 SWD/J-Link 的情况下收窄卡点。

启动阶段标记不能在正常循环中频繁写 flash。优先使用 reset-retained 寄存器；只有需要跨真实掉电保留的统计才使用低频、磨损受控的 NVS 记录。

## 启动循环定义

本轮自动验证包含两种不同循环，结果不得混写：

- `kernel reboot cold`：验证 Zephyr 冷复位路径、驱动重新初始化和 USB 重新枚举。
- 1200-baud→UF2 copy→application：验证 bootloader/app 切换、自动烧录和应用启动。

- `uhubctl` VBUS off→on：在 udev 权限和逐端口供电能力确认后，关闭精确映射的单个端口至少 2 秒，再恢复供电；每次只操作一块板并验证另一块板保持在线。

在 `uhubctl` 能实际切断整个 hub VBUS 时，第三种循环覆盖双板联合真实断电；它不能证明单板隔离故障。若 `uhubctl` 仍卡住，则由用户手动关闭/打开整个 hub，agent 监控两块板重新枚举、入网和重锁结果。

## 通过标准

某阶段只有同时满足以下条件才可继续：

- 对应新增测试先失败、实现后通过。
- 全部相关 native 测试和静态检查通过。
- pristine 固件构建无新增 warning/error。
- 指定启动循环全部完成，无 USB 丢失、watchdog reset 或 shell 失联。
- 双板功能计数符合该阶段预期，无未解释的 drop、restart、retry exhausted 或 sync 发散。
- 验证结果文档包含可重放命令、固件 SHA、设备 ID、循环次数、关键日志和明确结论。

若出现概率性失败，必须保留失败次数与总次数，不能用后续成功覆盖失败记录。修复后从失败阶段完整重跑，不只补跑剩余次数。

## 最终交付

最终交付包含：

- 渐进迁移后的固件和验证专用、默认关闭的诊断能力。
- 自动构建、串号绑定烧录、启动循环、CDC 监控和双板 bridge 数据测试工具。
- 软件测试结果、每阶段板测结果、冷复位统计和已知硬件覆盖缺口。
- 对原冷启动卡死根因的证据结论；若仍未触发，则报告置信范围和已排除的功能边界，不把“未复现”表述为“根因已修复”。
