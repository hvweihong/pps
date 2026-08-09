# Incremental board end-to-end validation results

验证分支：`validation/incremental-board-e2e`

固定设备映射：

- `DBE5C3D84EA2EC6F`：master
- `5B3D71D27A709CA2`：slave

## Stage 0 — main software baseline

执行日期：2026-08-08

- HEAD：`61b5cae8725e2fe29237159c5b721378ff9576e6`
- `git merge-base main HEAD`：同一提交
- NCS/Zephyr：构建输出为 Zephyr `4.3.99`、NCS build `v3.3.4`；native banner 为 NCS `v3.3.1-1d7a0b0e49b8`。
- native sync test：通过，14/14 cases，0 failed。
- 现有静态检查：全部通过。

### Pristine firmware builds

| role | build command | FLASH used | RAM used | UF2 size | result |
| --- | --- | ---: | ---: | ---: | --- |
| master | `./build.sh master` | 183436 B | 47536 B | 367104 B | PASS |
| slave | `./build.sh slave` | 175432 B | 47344 B | 351232 B | PASS |

两份构建均生成 `zephyr.elf`、`zephyr.hex` 和 `zephyr.uf2`。当前仅完成软件基线；固定 ID 烧录和板级循环将在 recovery stage 工具迁移后执行。

## Stage 8 — complete bridge, CDC validation and wireless time sync

执行日期：2026-08-09

阶段提交：

- `66fd246`：完整 ESB bridge、无线同步/PPS、恢复工具、诊断与动作提交失败回滚。
- `0aac2de`：CDC validation fail-closed 容量控制及生产/验证配置拆分。

### Host verification

- bridge native suite：93/93 cases，0 failed。
- sync native suite：15/15 cases，0 failed。
- UF2 Python suite：9/9 cases，0 failed。
- 所有现役 `tests/*_static_check.sh`：PASS；旧 BLE/MPSL 专用检查按设计 SKIP。
- production pristine build：`build/final-production/zephyr/zephyr.uf2`，333824 B，
  `CONFIG_RADIO_BRIDGE_VALIDATION_CDC` 未启用，ELF 不包含 validation shell symbol。
- validation pristine build：`build/final-validation/zephyr/zephyr.uf2`，338944 B，
  `CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y`，ELF 包含 `bridge_runtime_validation_inject`
  和 `cmd_bridge_test_inject`。

### Fixed-ID flashing and automatic recovery

- `DBE5C3D84EA2EC6F`：validation UF2 3/3 cycles，全部 attempt 1，固定 ID 返回 application。
- `5B3D71D27A709CA2`：validation UF2 3/3 cycles，全部 attempt 1，固定 ID 返回 application。
- 每个角色执行 3 次 `kernel reboot cold`。每次重启均以 `kernel uptime` 回退到约
  2.7 s 证明，未要求人工双击或拔插。
- 每次 master 重启后 slave 都重新报告 `sync_state=2`；每次 slave 重启后 master 都重新报告
  `active_count=1`。
- watchdog 为 30 s，1200-baud UF2 recovery 在双板上均已实际完成多轮循环。

### Runtime state and wireless synchronization

- master：`active_count=1`、`suspect_count=0`、`action_error_count=0`、
  `queue_drop_bytes=0`、`invalid_session_packets=0`、UART RX/TX drop 均为 0。
- slave：`slave_active=1`、`slave_node_id=1`、`sync_state=2`、
  `sync_missed_count=0`、`sync_filter_error_count=0`、`sync_tracker_error_count=0`、
  `sync_pps_reset_error_count=0`、`sync_last_pps_reset_error=0`、UART RX/TX drop 均为 0。
- 15 s peer-online 稳态窗口内，以上错误计数以及 duplicate 计数的增量均为 0。
- master 的 `radio_retry_exhausted` 只在先启动 master、后刷写/重启 slave 的故意 peer-offline
  窗口累计；peer-online 稳态窗口内不增长，且 `action_error_count` 始终为 0，不再出现启动时
  `SEND_POLL/-ENOMEM` 提交错误洪泛。

### CDC and physical UART bridge path

- master → slave：`bridge_test inject 600 49`，slave
  `bridge_test verify ok len=600 seed=49`，`captured=600`、`output_drop=0`。
- slave → master：`bridge_test inject 600 114`，master
  `bridge_test verify ok len=600 seed=114`，`captured=600`、`output_drop=0`。
- 3 轮双板 cold reboot 后，再次执行上述两个方向，均 exact verify 通过。
- CDC 接收数据仍同时提交给物理 UART TX：最终 master `uart_tx_bytes=600`，slave
  `uart_tx_bytes=1200`，两板 `uart_tx_drop_bytes=0`。本轮未连接外部物理 UART 适配器，
  因此未要求板外 RX/TX 电气回环。
- 反向 CDC 测试期间 master 统计到 3 个 uplink retransmit duplicate；payload 只交付一次，
  exact verify 仍通过，随后稳态窗口 duplicate 不再增长。

### NVS cleanup

- NVS 持久化和 `[100, 60000]` 范围拒绝测试已完成。
- slave 上用于测试的 `status_interval_ms=2000` 覆盖值已通过
  `param clear status_interval_ms` 清除。
- 最终两板均报告 `status_interval_ms = 1000 (default, runtime)`。
