# Unified Build CLI Design

## Goal

移除 `build.sh` 中已经失去固件语义的 `master|slave` 位置参数，使构建接口与“同一份固件由
NVS `role_id` 决定角色”的实现保持一致。

## Interface

- 统一使用 `./build.sh [options] [-- extra west build args]`。
- 默认输出目录固定为 `build/`；需要区分 production、validation 等产物时显式使用
  `-d/--build-dir`。
- 删除 `MODE`、`master|slave` 参数解析、角色目录分支和 mode 日志。
- 未经 `--` 分隔的未知参数立即报错，不再隐式传给 `west build`；额外 west 参数必须放在
  `--` 之后。
- README 的当前构建示例不再包含角色参数。

历史设计、计划和验证结果记录的是当时真实执行的命令，保持不变。

## Verification

- host 测试确认帮助信息只描述统一接口，且 `master`/`slave` 位置参数会失败。
- host 全量回归和 ruff 通过。
- production 与 validation 使用无角色命令完成 pristine build，并继续产生 ELF/HEX/UF2。
- 两个构建生成相同的统一角色运行时逻辑；角色仍只通过 NVS `role_id` 配置。
