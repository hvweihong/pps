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
