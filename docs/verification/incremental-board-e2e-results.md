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

## Stage 1 — reusable fixed-ID dual-board hardware gate

执行日期：2026-08-09

### Source and clean software baseline

- 分支：`validation/incremental-board-e2e`；基线 HEAD：
  `dd23544771c8405df7f84a1a9dc5d3e996aae402`。
- Stage 1 reusable gate 初始实现提交：
  `21749cf37329beed891bc15a8bf5cc3857de6b94`（`test: add reusable dual-board hardware stage gate`）。
- `git merge-base --is-ancestor effe24c HEAD` 返回 0；已批准设计提交是基线祖先。
- 开始 Stage 1 时 worktree clean。native 构建报告 Zephyr `4.3.99`、NCS build
  `v3.3.4`、host-tools `0.17.4`、host GCC `15.2.0`；运行 banner 为 NCS
  `v3.3.1-1d7a0b0e49b8`、Zephyr `v4.3.99-37e6c28576ee`。
- bridge native suite：以下命令 PASS，93/93 cases，0 failed：

  ```sh
  /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
    tests/bridge_logic -d build/tests/bridge_logic
  ./build/tests/bridge_logic/bridge_logic/zephyr/zephyr.exe
  ```

- sync native suite：以下命令 PASS，15/15 cases，0 failed：

  ```sh
  /home/hv/ncs/.venv/bin/west build -p always -b native_sim/native \
    tests/sync_logic -d build/tests/sync_logic
  ./build/tests/sync_logic/sync_logic/zephyr/zephyr.exe
  ```

- `for script in tests/*_static_check.sh; do bash "$script"; done`：13 PASS、
  15 个 retired BLE/MPSL 检查按配置 SKIP、0 FAIL，共 28 个脚本。
- Python gate/UF2 suite：

  ```sh
  /home/hv/ncs/.venv/bin/python -m unittest -v tests/test_board_e2e.py
  /home/hv/ncs/.venv/bin/python -m unittest -v tests/test_flash_uf2.py
  ```

  最终分别为 27/27 和 11/11 tests PASS。timeout 单测证明只恢复一次、只对 affected
  board 执行一次 flash attempt、再重试命令一次；第二次 timeout 保留 receive tail
  并抛错，不会无限循环。runtime drop tests 覆盖三字段全零、任一字段非零和任一字段缺失，
  并验证只接受 bridge traffic 之后的新 status sample。额外覆盖 recovery 后 reflash、CDC
  reconnect 和 command retry 的完整顺序，role 不变时不重启、role 改变时 cold reboot，
  timestamped raw log 落盘，以及 CLI fail-closed 路径。

### Fresh validation firmware

执行命令：

```sh
./build.sh master -d build/stage1-validation -- -DOVERLAY_CONFIG=validation.conf
```

- pristine build PASS；Zephyr `4.3.99`、NCS `v3.3.4`、Zephyr SDK/toolchain
  `0.17.4`、ARM GCC `12.2.0`。
- `CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y`；FLASH `169440 B`，RAM `188340 B`；
  UF2 `338944 B`。
- UF2 SHA-256：`f50a5fa4ade1e7fc3de225b862386bbd5e2c6d4df993bef88d76091546cae395`。
- 编译定义中的固件身份为 `APP_GIT_VERSION="dd23544771c8"`，没有 `-dirty`。
  配置时 Stage 1 的 Python 文件尚为 untracked；现有 CMake dirty 检测使用
  `git diff --quiet HEAD --`，因此它们不会改变固件 identity。固件 C 源和所有 tracked
  build inputs 对该提交均 clean。

### Fixed-ID hardware gate

最终权威命令：

```sh
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage1-baseline \
  --master-id DBE5C3D84EA2EC6F \
  --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage1-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage1-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --command-timeout 10
```

原始时间戳 CDC/evidence logs：
`build/board-e2e/20260809T041821.878308Z-stage1-baseline/`。

- 固定映射：master `DBE5C3D84EA2EC6F`，slave `5B3D71D27A709CA2`；所有 discovery、
  flash 和 CDC reconnect 均使用 `/dev/serial/by-id` 完整 ID，未按 `ttyACM` 编号选择。
- 两板均在 flash attempt 1/2 成功，application 精确 ID 重新枚举；flash retry 计数均为 0。
  最终 gate 没有 command timeout，automatic reflash recovery 计数均为 0；未使用
  `uhubctl`，也未要求手动 reset、拔插或 bootloader 双击。
- NVS role 检查：master `role_id=0`、slave `role_id=1`，均为 persisted 且已正确，
  因此本轮无需改值或 cold reboot。shell readiness 后 `kernel uptime` 分别报告 master
  `8347 ms`、slave `3844 ms`，随后两板 `bridge_test clear ok`。
- 无线状态：master `active_count=1`；slave 同时在 status log 报告 `sync_state=2`，
  `time_sync status` 报告 `LOCKED`。
- master → slave：`bridge_test inject 600 49`，slave 精确返回
  `bridge_test verify ok len=600 seed=49`。
- slave → master：`bridge_test inject 600 114`，master 精确返回
  `bridge_test verify ok len=600 seed=114`。
- 两板 `bridge_test stats` 均为 `input_drop=0 output_drop=0`；最终 runtime status 均为
  `uart_rx_drop_bytes=0`、`uart_tx_drop_bytes=0`、`queue_drop_bytes=0`。
- 最终输出：`BOARD_E2E PASS stage=stage1-baseline cycles=1 length=600`；结果 **PASS**。

### Review follow-up rerun

针对 `21749cf37329beed891bc15a8bf5cc3857de6b94` 的 review 修正后，重新执行完整软件和
硬件验证。fresh validation build 仍使用：

```sh
./build.sh master -d build/stage1-validation -- -DOVERLAY_CONFIG=validation.conf
```

- pristine build PASS；`CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y`，FLASH `169440 B`，
  RAM `188340 B`，UF2 `338944 B`。构建定义中的固件身份为
  `APP_GIT_VERSION="de3eef248e8a-dirty"`；UF2 SHA-256 为
  `f50a5fa4ade1e7fc3de225b862386bbd5e2c6d4df993bef88d76091546cae395`。
- bridge native suite 93/93、sync native suite 15/15 均 PASS；静态检查 13 PASS、
  15 intentional SKIP、0 FAIL；Python gate/UF2 suite 分别 27/27 和 11/11 PASS，
  `ruff` 与 `git diff --check` PASS。
- `findmnt`、`lsblk` 和 `udisksctl` 共用的 host subprocess 入口现在强制 10 s timeout，
  `TimeoutExpired` 转换为 `FlashError`，因此 mount/discovery 命令不能永久挂起。CLI
  bridge length 上限从 4096 对齐 firmware
  `RB_VALIDATION_BUFFER_SIZE=2048`，`2049` fail-closed 回归测试通过。
- review 后固定 ID gate 仍使用上面的权威命令和固定 `600` byte 长度；原始 evidence 位于
  `build/board-e2e/20260809T045917.332837Z-stage1-baseline/`。
- master 和 slave 均在 flash attempt 1/2 成功；flash retry 和 command-timeout recovery
  计数均为 0。role 分别为 persisted `role_id=0` 和 `role_id=1`；role 检查后的首次
  `kernel uptime` 分别为 `8333 ms` 和 `3786 ms`。
- 本轮 readiness 要求同一次轮询中的当前 master `active_count=1` 与当前 slave
  `sync_state=2`/`LOCKED` 配对成立，且只采用各 fresh suffix 中最后一个 readiness 值，
  不能由历史状态或同一 suffix 内较早的 ready 值粘滞满足。ready 后紧跟 not-ready 的
  冲突样本回归测试通过。
- master → slave 的 `len=600 seed=49` 和 slave → master 的 `len=600 seed=114` 均
  exact verify PASS；两板 `bridge_test stats` 均为 `input_drop=0 output_drop=0`。
- bridge traffic 后等待新的周期 status：master 在 firmware uptime `10.037 s`、slave 在
  `6.035 s` 报告新样本；两板均为 `uart_rx_drop_bytes=0`、`uart_tx_drop_bytes=0`、
  `queue_drop_bytes=0`。最终输出为 `BOARD_E2E PASS`，结果 **PASS**。

### Verification boundary

Stage 1 只验证 USB fixed-ID flash/re-enumeration、CDC shell、板上 runtime/status、无线同步状态和
双向 bridge payload。测试时未连接外部 GNSS/PPS 源或示波器/逻辑分析仪，因此**没有测量外部
PPS 相位，也没有验证或声明 `<= 20 us` 的外部 PPS 相位误差**。该指标必须在后续带外部 PPS
参考和测量仪器的阶段单独验证。

## Stage 2 — pure NMEA UTC parser and clock model

执行日期：2026-08-09

### TDD and host verification

RED evidence：先加入 `test_nmea_parser.c`/`test_utc_clock.c` 和 CMake sources 后，执行：

```sh
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always \
  -b native_sim/native tests/bridge_logic -d build/tests/bridge_logic
```

配置按预期失败，错误为 `Cannot find source file: ../../src/nmea_parser.c`。随后为 clock 补充的
语义 RED cases 也分别失败：`test_external_cold_boot_is_acquiring_but_utc_invalid`、
`test_utc_seconds_must_be_consecutive_to_lock`、`test_phase_reset_failure_does_not_accept_pair` 和
`test_publication_projects_to_its_now_tick`。

RMC approved field vector 中的 `0.0,0.0` payload XOR checksum 是 `5E`，不是标准示例中不同
speed/course fields 所对应的 `6A`；测试因此使用 checksum-valid `*5E`，并保留严格的 checksum
mismatch rejection，未放宽 parser 来接受错误的 `*6A`。

GREEN commands：

```sh
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always \
  -b native_sim/native tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/bridge_logic/zephyr/zephyr.exe

ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always \
  -b native_sim/native tests/sync_logic -d build/tests/sync_logic
./build/tests/sync_logic/sync_logic/zephyr/zephyr.exe

/home/hv/ncs/.venv/bin/python -m unittest -v \
  tests/test_board_e2e.py tests/test_flash_uf2.py
for script in tests/*_static_check.sh; do bash "$script"; done
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
git diff --check
```

- bridge native suite：110/110 PASS；其中新增 `nmea_parser` 6/6 和 `utc_clock` 11/11。
- sync native suite：15/15 PASS。
- Python board-gate/UF2 suite：38/38 PASS。
- static checks：13 PASS、15 retired BLE/MPSL intentional SKIP、0 FAIL。
- `ruff` 和 `git diff --check`：PASS。shell-path `ruff` 不在 `PATH`，故使用已安装的
  `/home/hv/ncs/.venv/bin/python -m ruff`。

Clock publication 的 `now_tick` 是纯投影：不改变 model state，而是以该 tick 推进复制状态后
返回 next PPS UTC second/quality；该语义由 `test_publication_projects_to_its_now_tick` 覆盖。

### Fresh validation firmware

```sh
./build.sh master -d build/stage2-validation -- -DOVERLAY_CONFIG=validation.conf
sha256sum build/stage2-validation/zephyr/zephyr.uf2
```

- pristine build PASS；`CONFIG_RADIO_BRIDGE_VALIDATION_CDC=y`；FLASH `169440 B`，RAM
  `188340 B`，UF2 `338944 B`。
- UF2 SHA-256：`f50a5fa4ade1e7fc3de225b862386bbd5e2c6d4df993bef88d76091546cae395`。
- 构建时 worktree 含 Stage 2 tracked/untracked changes；编译定义为
  `APP_GIT_VERSION="bee49765839e-dirty"`。

### Fixed-ID hardware regression gate

权威 gate command：

```sh
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage nmea-utc-pure \
  --master-id DBE5C3D84EA2EC6F \
  --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage2-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage2-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --command-timeout 10
```

- master `DBE5C3D84EA2EC6F` 与 slave `5B3D71D27A709CA2` 均用完整
  `/dev/serial/by-id` identity flash/reconnect；flash attempt 为 1/2，retry/recovery 都是 0。
- roles 为 0/1；master `active_count=1`；slave `sync_state=2/LOCKED`。
- master → slave `len=600 seed=49` 与 slave → master `len=600 seed=114` 均 exact verify PASS。
- 两板 post-traffic `bridge_test stats` 均 `input_drop=0 output_drop=0`，fresh runtime status
  均 `uart_rx_drop_bytes=0 uart_tx_drop_bytes=0 queue_drop_bytes=0`。
- 输出：`BOARD_E2E PASS stage=nmea-utc-pure cycles=1 length=600`。

在同一 fixed-ID CDC sessions 中，gate 后独立执行 `kernel uptime`、`bridge_test stats`，等待
31 秒，再重复同样命令并等待新的 runtime status；每个采样都重新解析 fresh output，而非复用
gate 历史样本。窗口结果：

- master：`40038 -> 74588 ms`，delta `34550 ms`；start/end 均
  `active_count=1`，bridge/UART/runtime drops 均为 0。
- slave：`36503 -> 71595 ms`，delta `35092 ms`；start/end 均 `sync_state=2/LOCKED`，
  bridge/UART/runtime drops 均为 0。
- 单调 uptime 跨越完整 30 s 窗口，未发生 watchdog/reset；firmware 没有单独可读的 watchdog
  reset counter，因此这是 reset absence 的直接 CDC evidence。

最终 raw logs：
`build/board-e2e/20260809T052816.055793Z-nmea-utc-pure/master.raw.log`、
`build/board-e2e/20260809T052816.055793Z-nmea-utc-pure/slave.raw.log`、
`build/board-e2e/20260809T052816.055793Z-nmea-utc-pure/master.steady-30s.raw.log` 和
`build/board-e2e/20260809T052816.055793Z-nmea-utc-pure/slave.steady-30s.raw.log`。

### Verification boundary

本阶段的 NMEA parser/UTC clock 是未接入 GPIO/UART/Zephyr 的 pure model；双板 gate 证明将这些
模块链接进 validation firmware 后，既有固定-ID CDC/ESB bridge、无线 `LOCKED` 状态与 600-byte
双向 traffic 没有回归。没有接入外部 GNSS NMEA 或 PPS 信号，也没有连接示波器/逻辑分析仪；因此
external PPS capture、NMEA association 与 `<=20 us` phase accuracy 均**未验证且未声明**，留给
Stage 3 硬件接入和测量。

### Stage 2 review follow-up — atomic recovery and bounds hardening

在 `af4f0697cc3a61b2df12e546add816293d7323d1` 的 code-quality review 后，补充了以下 RED
tests：`test_discontinuous_pair_restarts_acquisition`、
`test_failed_discontinuous_reset_preserves_old_baseline`、
`test_zda_requires_fixed_width_date_fields`、`test_failure_does_not_mutate_utc_output`、
`test_overflowing_pair_is_rejected_without_state_change` 与
`test_backward_ticks_do_not_regress_clock_or_publication`。初始 run 全部按预期失败；实现后 bridge
native suite 为 **116/116 PASS**（`nmea_parser` 8/8，`utc_clock` 15/15）。

- ACQUIRING/LOCKED 中 timely but discontinuous pair 现在原子地成为新的 phase-reset acquisition
  baseline；随后三个连续 pairs 才 lock。phase-reset callback 失败保持旧 baseline，不能半更新。
- ZDA 强制 `dd,mm,yyyy` 宽度；parser 仅在完整有效 parse 后写入 caller UTC output。
- UTC clock 忽略 backwards `tick()`，拒绝 backwards publication projection，并对 PPS target/tick
  progression/signed UTC second additions做 overflow guards。
- 复跑 sync native：15/15 PASS；Python：38/38 PASS；active static：13 PASS、retired skips：15；
  `ruff` 与 `git diff --check` PASS。

最终 fresh validation command：

```sh
./build.sh master -d build/stage2-validation-review -- -DOVERLAY_CONFIG=validation.conf
```

- FLASH `169440 B`，RAM `188340 B`，UF2 `338944 B`；SHA-256 仍为
  `f50a5fa4ade1e7fc3de225b862386bbd5e2c6d4df993bef88d76091546cae395`。
- build identity：`APP_GIT_VERSION="af4f0697cc3a-dirty"`。

最终 fixed-ID command 使用 `build/stage2-validation-review/zephyr/zephyr.uf2`：

```sh
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage nmea-utc-pure-review \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage2-validation-review/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage2-validation-review/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --command-timeout 10
```

- gate PASS：roles 0/1，master `active_count=1`，slave `sync_state=2/LOCKED`，两个 600-byte
  directions exact verify，bridge/UART/queue drops 全为 0，flash/recovery/retry 全为 0。
- 独立 fresh CDC 31-second window：master `39043 -> 73568 ms` (delta `34525 ms`)；slave
  `35484 -> 70563 ms` (delta `35079 ms`)；start/end 都重新检查 ready state 和 zero drops。
- 最终 raw logs：
  `build/board-e2e/20260809T054517.485999Z-nmea-utc-pure-review/master.raw.log`、
  `build/board-e2e/20260809T054517.485999Z-nmea-utc-pure-review/slave.raw.log`、
  `build/board-e2e/20260809T054517.485999Z-nmea-utc-pure-review/master.steady-30s.raw.log`、
  `build/board-e2e/20260809T054517.485999Z-nmea-utc-pure-review/slave.steady-30s.raw.log`。

## Stage 3 — external PPS capture and NMEA UART input

执行日期：2026-08-09。基线为 `85d2a23f5c5a68e8cb4b1a8d9e7dffc322e48a2f`；所有下列
firmware artifacts 都在该基线上带 Stage 3 未提交改动（dirty）构建。

### Host verification

- bridge native suite：**117/117 PASS**；sync native suite：**15/15 PASS**。
- Python board-gate/UF2 suite：**43/43 PASS**。
- `for script in tests/*_static_check.sh; do bash "$script"; done`：10 个 active checks
  PASS，15 个 retired BLE/MPSL checks 按配置 SKIP，0 FAIL。
- `/home/hv/ncs/.venv/bin/python -m ruff check tests tools` 与 `git diff --check`：PASS。
- `tests/test_board_e2e.py` 将 CDC rolling history 改为 absolute cursor；回归测试覆盖
  boot-guard marker、radio readiness、runtime drops 与 time-UART errors 在历史头部淘汰后仍只
  读取新样本的情况。

执行命令：

```sh
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always \
  -b native_sim/native tests/bridge_logic -d build/tests/bridge_logic
./build/tests/bridge_logic/bridge_logic/zephyr/zephyr.exe
ZEPHYR_BASE=/home/hv/ncs/zephyr /home/hv/ncs/.venv/bin/west build -p always \
  -b native_sim/native tests/sync_logic -d build/tests/sync_logic
./build/tests/sync_logic/sync_logic/zephyr/zephyr.exe
/home/hv/ncs/.venv/bin/python -m unittest -v tests/test_board_e2e.py tests/test_flash_uf2.py
for script in tests/*_static_check.sh; do bash "$script"; done
/home/hv/ncs/.venv/bin/python -m ruff check tests tools
git diff --check
```

### Fresh firmware builds and DTS evidence

| build | command | FLASH | RAM | UF2 |
| --- | --- | ---: | ---: | --- |
| production master | `./build.sh master -d build/stage3-master` | 168704 B | 186228 B | 337408 B, `61673ce7cce211650d512f77ceb99852379523cddfcd29cd5b830e8efe67ea09` |
| production slave | `./build.sh slave -d build/stage3-slave` | 168704 B | 186228 B | 337408 B, `61673ce7cce211650d512f77ceb99852379523cddfcd29cd5b830e8efe67ea09` |
| validation (runtime-wake) | `./build.sh master -d build/stage3-validation -- -DOVERLAY_CONFIG=validation.conf` | 171432 B | 190388 B | 343040 B, `8a24e34f10b907233dcfe76d664318911aa53c6377b7b8e8a3fb527f9e965abc` |
| validation (stopped-event fix) | `./build.sh master -d build/stage3-validation -- -DOVERLAY_CONFIG=validation.conf` | 171588 B | 190388 B | 343552 B, `76fc9802d753e0e35ddfe2569535016db57d84e15290b2b9bd1ad5f6a258e3f9` |

Production master/slave images were not rebuilt for the stopped-event correction; their rows
remain the earlier runtime-wake artifacts. The fixed validation UF2 is the artifact used below.

`build/stage3-master/zephyr/zephyr.dts` confirms `time-uart = &uart1` and `pps-in =
&pps_in` (lines 35 and 38), UART1 at 9600 baud with default/sleep pinctrl (lines 625–637),
P0.04 TX / P0.05 pull-up RX encoded in `uart1_default` (lines 934–960), and PPS input GPIO0
P0.02 (lines 1080–1084).

### Reboot-gate investigation and final hardware result

Earlier failed evidence is intentionally retained:

- `build/board-e2e/20260809T062226.103068Z-stage3-reboots-authoritative/` proved that a
  second reboot before the 10 s persisted radio boot-guard clear interval enters the
  guard-recovery safety loop.
- `build/board-e2e/20260809T063038.274182Z-stage3-authoritative/` was externally interrupted
  during slave reboot 1/5; it has no `BOARD_E2E FAIL` record and does not constitute a pass.
- `build/board-e2e/20260809T064513.579709Z-stage3-authoritative-final/` reached slave reboot
  5/5 and physically logged the clear marker, but the old length-based fresh-history slice missed
  it after the 256 KiB rolling history evicted its head. This run is a failure, not combined with
  any partial result.

The earlier cursor run
`build/board-e2e/20260809T065445.948488Z-stage3-authoritative-final-cursor/` remains
preserved but is superseded after the Important wake-registration correction connected the
time-UART ring ingestion to the bridge worker wake callback. The corrected integration was
verified by one new complete command, with a 600 s outer timeout:

```sh
/usr/bin/timeout 600s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage3-authoritative-final-runtime-wake \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage3-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage3-validation/zephyr/zephyr.uf2 \
  --cycles 1 --cold-reboots-per-board 5 --steady-state-seconds 30 \
  --bridge-length 600 --command-timeout 10
```

- Result: **`BOARD_E2E PASS`** after 242.8 s; exact-ID flashes master/slave 1/1, no flash retry,
  no command recovery.
- Master 5/5 then slave 5/5 each recorded CDC re-enumeration, reduced post-reset uptime,
  `radio boot guard cleared after stable startup` after at least 10 s, peer readiness
  (`active_count=1` / `LOCKED`), zero bridge queue drops, and
  `time_uart rx_restart_errors=0 rx_buffer_errors=0`.
- Post-reboot exact 600-byte traffic passed in both directions; `--steady-state-seconds 30`
  repeatedly rechecked radio readiness, runtime drops and time-UART errors, then recorded PASS on
  both boards.
- Final raw evidence:
  `build/board-e2e/20260809T071042.917744Z-stage3-authoritative-final-runtime-wake/master.raw.log`
  and
  `build/board-e2e/20260809T071042.917744Z-stage3-authoritative-final-runtime-wake/slave.raw.log`。

The above 5+5 cold-reboot and 30 s steady-state result belongs to the runtime-wake firmware. It is
preserved as historical robustness evidence, but is superseded as the authoritative Stage 3
firmware result by the stopped-event correction below.

### Stopped-event correction hardware result

`UART_RX_STOPPED` now records an atomic-safe event count and reason mask before it conditionally
copies only non-NULL, non-empty RX data. The board gate requires zero for every reported input
drop/error field: `rx_drop_bytes`, `overlong_line_drops`, `output_line_drops`,
`rx_restart_errors`, `rx_buffer_errors`, `rx_stopped_events`, `rx_stop_reason_mask`, and
`pps_input_drop_count`.

The corrected validation UF2 was verified with this standalone command:

```sh
/usr/bin/timeout 600s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage3-time-uart-stopped-fix-short-gate \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage3-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage3-validation/zephyr/zephyr.uf2 \
  --cycles 1 --cold-reboots-per-board 1 --steady-state-seconds 15 \
  --bridge-length 600 --command-timeout 10
```

- Result: **`BOARD_E2E PASS`** after 89 s; exact-ID flash master/slave 1/1, zero flash retries,
  and zero automatic command recoveries.
- This corrected-firmware run completed one cold reboot of each board, exact 600-byte traffic in
  both directions before and after reboot, and a 15 s steady-state window. It is intentionally not
  presented as a replacement 5+5/30 s endurance result.
- Authoritative raw evidence:
  `build/board-e2e/20260809T073739.138526Z-stage3-time-uart-stopped-fix-short-gate/master.raw.log`
  and
  `build/board-e2e/20260809T073739.138526Z-stage3-time-uart-stopped-fix-short-gate/slave.raw.log`。

The ISR ring ingestion and bridge worker wake are connected, but this stage does not yet wire
NMEA parsing or any `time_uart_read_line()` consumer. That work is intentionally deferred to
Task 4.

### Verification boundary

No external PPS source, GNSS NMEA source, oscilloscope, or logic analyzer was wired for this
stage. The result validates firmware integration, CDC transport, reboot safety, and the enumerated
software input drop/error counters; it makes no claim that physical PPS capture, NMEA
reception/association, or electrical timing accuracy has been measured.

## Stage 4 — UTC publication and time-source states

执行日期：2026-08-09。基线为 Stage 3 提交 `52695d8`。本阶段将无线同步协议升级为
v2/65-byte sync frame，传播 `next_pps_utc_seconds` 与 `time_quality`，并接入 master 的
`LOCAL`、`EXTERNAL_ACQUIRING`、`EXTERNAL_LOCKED`、`EXTERNAL_HOLDOVER` 状态。新增
`time_source_mode`（0 local、1 external）与 `pps_input_delay_us` 持久参数；validation firmware
提供 `time_test pair` 和 `time_test source_lost`，只在 bridge thread 上模拟配对/老化。

### Host/build evidence

- bridge native suite：**118/118 PASS**；sync native suite：**16/16 PASS**。
- Python board/flash suite：**46/46 PASS**；11 个 active static checks PASS，15 个 retired
  BLE/MPSL checks SKIP；`ruff` 与 `git diff --check` PASS。
- `tests/time_source_integration_static_check.sh` PASS，覆盖 group-first reject、UTC publication、
  filter age/drift，以及 PPS/NMEA 仅在 external master 初始化。
- flash helper 回归：**14/14 PASS**。实板发现 CDC line coding 已为 1200 时，重复 1200-touch
  不触发 bootloader；工具现先写 115200 再写 1200，后续两板均首刷成功。
- validation build：FLASH `178192 B`、RAM `190836 B`、UF2 `356864 B`，SHA-256
  `f5a7567c013ed1ebe757adab49354fe23872424d934ecd5135bc2aa453da2d10`。

### Hardware findings retained

- `build/board-e2e/20260809T081818.916393Z-stage4-precondition-rerun/` 在 slave 的 UF2 枚举
  上失败；内核日志证明 1200-touch 后没有 USB disconnect。显式 115200→1200 后同一固定 ID
  单次 flash 成功，由上述 TDD 回归固化。
- `build/board-e2e/20260809T083013.907150Z-stage4-time-state-resilient-gate/` 在
  `EXTERNAL_HOLDOVER` 和双向 600-byte traffic 均通过后，正确拒绝 slave 的
  `pps_input_drop_count=75`。根因是未接线 P0.02 浮空且 PPS input 被所有角色初始化；现改为只在
  external master 初始化 PPS input 与 NMEA UART。

修复后先执行标准 fixed-ID gate：

```sh
/usr/bin/timeout 300s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage4-external-input-role-gated \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage4-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage4-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --command-timeout 10
```

- **`BOARD_E2E PASS`** after 16.6 s；两板 flash attempt 1/2 成功，零 retry/recovery；无线
  `active_count=1`/`LOCKED`，双向 600-byte exact verify，所有 UART/PPS/queue drops 为 0。
- Raw evidence：
  `build/board-e2e/20260809T083323.619638Z-stage4-external-input-role-gated/master.raw.log`、
  `build/board-e2e/20260809T083323.619638Z-stage4-external-input-role-gated/slave.raw.log`。

随后在同一 fresh firmware 上运行 Stage 4 专用 CDC 状态门禁：master 持久化
`time_source_mode=1` 并 cold reboot，验证 `EXTERNAL_ACQUIRING/UTC_INVALID`；连续提交
`1700000200..1700000202` 三个 synthetic pairs 后，两板分别达到
`EXTERNAL_LOCKED/LOCKED` 与 `WIRELESS/LOCKED`；`time_test source_lost` 后两板均传播
`HOLDOVER`。HOLDOVER 下双向 600-byte exact verify PASS。最后恢复
`time_source_mode=0` 并 cold reboot，两板恢复 `LOCAL/UTC_INVALID` 与
`WIRELESS/UTC_INVALID`，再次完成双向 600-byte exact verify，全部输入/queue drops 为 0。

- Result：**`STAGE4_TIME_GATE PASS`** after 22.8 s。
- Authoritative raw evidence：
  `build/board-e2e/20260809T083433.640824Z-stage4-time-state-final-short/master.raw.log`、
  `build/board-e2e/20260809T083433.640824Z-stage4-time-state-final-short/slave.raw.log`。
- Gate cleanup 再次读取 NVS，确认 master 的 `time_source_mode=0`，未把 external test mode
  留给后续阶段。

### Verification boundary

本阶段的 external state transition 使用 CDC synthetic pair，验证的是板上 thread-context
NMEA/PPS association、UTC 状态机与无线传播，不代表物理 P0.02/P0.05 电气输入已测量。仍无外部
GNSS/PPS 源或示波器，因此不声明真实输入捕获或 `<=20 us` 相位精度。

## Stage 5 — Best-effort broadcast downlink

执行日期：2026-08-09。基线为 Stage 4 提交 `d054fb6`。本阶段将 master 业务下行改为
`no_ack` 广播，删除 ACK_UPLINK 中的下行 epoch/base/bitmap、master 下行 history、repair 与
skip action；ASSIGN、POLL 和 slave→master 可靠轮询上行保持不变。slave 对新 sequence 立即
交付完整 payload，缺号只累计 `downlink_gap`，重复/旧帧只累计 `downlink_duplicate`，不等待补包。

### Host/build evidence

- TDD RED：新增 gap/duplicate 测试首先因 scheduler 尚无对应 sequence-gate 计数而编译失败。
- bridge native suite：**121/121 PASS**；sync native suite：**16/16 PASS**。
- Python board/flash suite：**48/48 PASS**；全部 active static checks、`ruff` 与
  `git diff --check` PASS。
- `tests/radio_transport_static_check.sh` fail-closed 检查 ACK header 为 22 bytes、生产路径不存在
  repair/skip/history/downlink ACK bitmap、业务广播显式使用 `no_ack=true`，并要求 validation
  丢包发送生成 synthetic TX-success，避免 scheduler 永久停在 in-flight。
- validation + loss build：FLASH `177140 B`、RAM `159028 B`、UF2 `354304 B`，SHA-256
  `2a3456d10cc36f2d04996efcb85621d1f250f8f56d4871c62b5aba6cd6f52cf9`。

丢包注入只在 `loss-validation.conf` 启用。`bridge_test loss <frame_type> <every_n>` 仅对匹配
frame type 计数；主动丢弃 TX 时向 runtime 排入 synthetic TX-success，因此 validation 不会因
故意丢包卡住 transaction gate。`bridge_test loss_off` 将 mask/rate 清零。

### Dual-board hardware gate

执行命令：

```sh
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage5-best-effort-loss-short \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage5-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage5-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --downlink-loss-every-n 3 \
  --command-timeout 10
```

- **`BOARD_E2E PASS`** after 34.7 s；两板 exact-ID flash attempt 1/2 成功，零 flash retry、
  零 command recovery。
- injection off 时 master→slave 与 slave→master 各 600-byte exact verify PASS，无 UART、
  validation、time-UART 或 scheduler queue drop。
- master 对 `RB_FRAME_DOWNLINK_DATA`（type 4）每 3 帧丢 1 帧；连续两个不同 600-byte pattern
  后 slave `downlink_gap` 从 0 增至 2，调度器保持响应。
- loss active 期间 slave→master 600-byte exact verify PASS，证明可靠轮询上行未受业务下行
  丢包影响。
- `loss_off` 后清理 validation payload 队列，下一组 master→slave 600-byte pattern 立即 exact
  verify PASS；最终两板所有 input/output/runtime drops 仍为 0。
- Authoritative raw evidence：
  `build/board-e2e/20260809T091052.235923Z-stage5-best-effort-loss-short/master.raw.log`、
  `build/board-e2e/20260809T091052.235923Z-stage5-best-effort-loss-short/slave.raw.log`。

### Verification boundary

本阶段实板证明单 slave 下 best-effort 下行的丢包前进、可靠上行独立性与关闭注入后的恢复。
多 slave 的上行 record 隔离属于 Stage 6；外部 PPS/GNSS 与 `<=20 us` 相位测量边界保持不变。

## Stage 6 — Per-slave uplink records and record-aware UART TX

执行日期：2026-08-09。基线为 Stage 5 提交 `1851768`。本阶段将 master 的单一上行字节队列
替换为每 node 独立的 bounded record queue；ACK_UPLINK payload 只有在整条入队成功后才推进
该 node 的 `uplink_ack_base`。runtime 先 peek node record，UART 描述符整条接收后才 capture CDC
并 pop scheduler record；描述符满时保留 scheduler record，UART abort 只重试未发送后缀。

### Host/build evidence

- record queue 与 master round-robin TDD：整条 push/peek/pop、容量不足时不修改旧记录、
  node1/node2/node1 读取不拼接。
- bridge native suite：**124/124 PASS**，其中 record queue 新增 3/3 PASS。
- Python board/flash suite：**51/51 PASS**；全部 active static checks、`ruff` 与
  `git diff --check` PASS。
- validation + loss build：FLASH `179544 B`、RAM `192692 B`（73.51%）、UF2 `359424 B`，
  SHA-256 `78831fc611753bacbd67cb320d5f39f688cc94ef76e588dcaf8c38be2e308c29`。
- validation 新增 `node1_records/node1_bytes/node1_record_drop` 与 UART record
  queued/completed/aborted/rejected/pending/busy/start-error 诊断；`bridge_test loss_once 7`
  只丢下一条非空 ACK_UPLINK，并报告实际 drop count。

### Dual-board hardware gate

为减少重复刷写，同一次 fixed-ID flash 后通过 `--bridge-repeats 5` 连续验证五轮：

```sh
/usr/bin/timeout 300s /home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage6-record-uart-short \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage6-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage6-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-repeats 5 --bridge-length 600 \
  --uplink-ack-loss-once --command-timeout 10
```

- **`BOARD_E2E PASS`** after 55.4 s；两板各 flash 一次，零 flash retry、零 command
  recovery。
- 五轮 master→slave 与 slave→master 各 600-byte exact verify 全部 PASS；master 每轮收到
  node1 的 3 条 record，累计 `node1_records=15`、`node1_bytes=3000`。
- 每轮结束 UART record `queued=completed`、`pending=0`、`busy=0`、`aborted=0`，node1、UART、
  validation、time-UART 与 scheduler queue drop 全为 0。
- slave 一次性丢弃 1 条非空 ACK_UPLINK（type 7）后，额外 600-byte pattern 仍 exact verify；
  master 最终 `node1_records=18`、`node1_bytes=3600`，CDC output 无残留重复 payload。
- Authoritative raw evidence：
  `build/board-e2e/20260809T094249.807445Z-stage6-record-uart-short/master.raw.log`、
  `build/board-e2e/20260809T094249.807445Z-stage6-record-uart-short/slave.raw.log`。

### Verification boundary

双板实测证明单物理 slave 的 record attribution、完整 UART descriptor draining 与一次 ACK_UPLINK
丢失恢复；三 slave 的独立 round-robin/不拼接由 native test 覆盖。当前没有第二、第三块 slave，
因此不把多物理 slave 并发宣称为板级实测；外部 PPS/GNSS 和 `<=20 us` 相位边界保持不变。

## Stage 7 — Effective NVS parameters and group isolation

执行日期：2026-08-09。基线为 Stage 6 提交 `f5ba8f5`。本阶段修正 settings/NVS 缺失时的
默认值 fallback、持久化 mutex、32-hex group key shell、reboot/runtime flags，并把 UART baud、
radio delay、固定 1 Hz PPS、100 ms pulse width、runtime status/LED 与 early group reject 接到真实
consumer。`invalid_group_packets` 在任何 sync session/filter 状态变化前计数。

### Host/build evidence

- bridge native suite：**135/135 PASS**；sync native suite：**16/16 PASS**；Python
  board/flash suite：**51/51 PASS**。
- 全部 active static checks、`ruff` 与 `git diff --check` PASS。新增参数 fallback/bytes/flags、
  固定 ring/PPS 范围、radio delay 方向、group mismatch 不修改 slave 状态，以及 action failure
  必须让出 runtime 内层循环的回归门禁。
- validation build：FLASH `180196 B`、RAM `192692 B`（73.51%）、UF2 `360448 B`，最终
  SHA-256 `19b504ef4248be178ebbe6b30f91a40d20b80b7ac3186b6281346f7c5a4d54db`。

### ESB TX failure root cause and recovery

最初两轮 Stage 7 门禁中，master 均在启动约 0.4 s 后停在 `radio_tx_packets=63`、
`radio_retry_exhausted=8`、`sync_tx_capture_count=4`，随后每个 sync action 都由
`esb_write_payload()` 返回 `-ENOMEM`。将两板持久化 `pps_width_us` 从 100000 单独改为 100 后，
`build/board-e2e/20260809T103024.674615Z-stage7-pps-width-100-ab/` 仍以相同计数失败，故排除
100 ms PPS pulse width 为诱因。

Nordic ESB 在可靠 PTX 的 `TX_FAILED` 分支不会从 TX FIFO 弹出失败 payload；当前 runtime 的
transaction gate 每次只有一帧在途，但 slave 被随后重刷时连续 8 次 poll 失败，恰好填满
`CONFIG_ESB_TX_FIFO_SIZE=8`。transport 现于 `ESB_EVENT_TX_FAILED` 事件中调用
`esb_pop_tx()`，失败时输出明确 error log；静态回归要求该清理路径存在。修复后以完全相同的
master-first/slave-second 刷写顺序运行：

```sh
/home/hv/ncs/.venv/bin/python tools/board_e2e.py \
  --stage stage7-esb-tx-failed-recovery \
  --master-id DBE5C3D84EA2EC6F --slave-id 5B3D71D27A709CA2 \
  --master-uf2 build/stage7-validation/zephyr/zephyr.uf2 \
  --slave-uf2 build/stage7-validation/zephyr/zephyr.uf2 \
  --cycles 1 --bridge-length 600 --bridge-repeats 1 \
  --steady-state-seconds 5 --command-timeout 15
```

- **`BOARD_E2E PASS`** after 38.1 s；两板各 flash 一次，零 retry/recovery；无线恢复
  `active_count=1`/`LOCKED`，双向 600-byte exact verify 和所有 queue/drop 门禁通过。
- Raw evidence：
  `build/board-e2e/20260809T103645.184837Z-stage7-esb-tx-failed-recovery/master.raw.log`、
  `build/board-e2e/20260809T103645.184837Z-stage7-esb-tx-failed-recovery/slave.raw.log`。

### Persisted parameter and group hardware gates

两板均持久化并 cold reboot 验证：master/slave `role_id=0/1`、`uart_baudrate=115200`、
`pps_width_us=100000`、`time_source_mode=0`、`group_id=1`、
`group_key=00112233445566778899aabbccddeeff`。`param list` 对这些值均报告 `yes/reboot`，
未覆写的固定 period/ring 与 runtime status/LED 仍报告 compiled default 和准确 flags。

- 批准的 100 ms PPS 参数下，
  `build/board-e2e/20260809T103926.069039Z-stage7-approved-params-100ms/` 完成 exact-ID flash、
  `active_count=1`/`LOCKED`、双向 600-byte exact verify、5 s steady state，零 retry/recovery/drop。
- 只把 slave 改为 `group_id=2` 并 reboot 后，master 最新 `active_count=0`，slave
  `UNLOCKED` 且 `radio_rx_packets=0`；证据位于
  `build/board-e2e/20260809-stage7-group-isolation/`。
- 恢复 slave `group_id=1` 后，首次立即重刷在 10 s boot-guard clear 之前发生，按设计进入
  diagnostic safety loop；该失败保留于
  `build/board-e2e/20260809T104134.380373Z-stage7-group-restored/`，不计为通过。guard 自动删除后
  重跑 `build/board-e2e/20260809T104258.426861Z-stage7-group-restored-retry/`，再次达到
  `active_count=1`/`LOCKED`、双向 600-byte exact verify、5 s steady state，零 retry/recovery/drop。

提交前独立规格审查发现外部时间参数的稳定 NVS ID 与批准计划相反，并指出异组回归只覆盖了
scheduler。修复后 `time_source_mode/time_uart_baudrate/pps_input_delay_us` 分别固定为
`0x1011/0x1012/0x1013`；新增原生 runtime policy 测试验证异组帧只增加
`invalid_group_packets`，不改变 sync sequence、relock、UTC 等状态，同时静态门禁要求该策略在
session/filter/counter 修改之前执行。最终固件重新运行固定 ID 双板门禁：

- `build/board-e2e/20260809T110223.197347Z-stage7-review-fixes/`：**BOARD_E2E PASS**，
  `active_count=1`/`LOCKED`、双向 600-byte exact verify、5 s steady state，零
  flash retry、command recovery、queue/drop 与 time-UART error。

代码质量审查进一步指出旧固件曾使用反向的两个时间参数 ID；用户明确选择不做兼容迁移，直接在
两块板上清除旧布局。固定 ID CDC 操作已分别执行 `param clear time_source_mode` 与
`param clear time_uart_baudrate`，随后两板均报告 `0 (default)` 与 `9600 (default)`。同一轮审查还
发现 `status_interval_ms` 原最大值 60000 ms 会超过 30000 ms watchdog；新增 RED→GREEN 原生测试并
将上限收紧至 10000 ms。最终固件再次通过：

- `build/board-e2e/20260809T111825.465847Z-stage7-final-clean-ids-watchdog/`：
  **BOARD_E2E PASS**，`active_count=1`/`LOCKED`、双向 600-byte exact verify、5 s steady
  state，零 flash retry、command recovery、queue/drop 与 time-UART error。

### Verification boundary

本阶段通过板上 counter 与无线同步证明固定 1 Hz/100 ms PPS scheduler 持续运行，但仍没有示波器
或外部 PPS/GNSS fixture，因此不声明 `<=20 us` 电气相位精度。真实物理 UART 双向数据将在下一阶段
使用本机 `/dev/ttyACM2` 与 adb 端 `/dev/ttyS1` 单独验证；本阶段的 600-byte 链路仍使用 validation
CDC injection/capture。
