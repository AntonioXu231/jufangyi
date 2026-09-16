# pd_ddr_slot_mgr 设计决议回复

- **日期**：2026-09-16
- **工程**：`pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`
- **对应文档**：`pd_ddr_slot_mgr_decisions_20260916.md`
- **状态**：D1、D2、D3、D4、D5 均已确认，可据此更新接口契约并进入 RTL 骨架编写阶段。

---

已阅读并认可设计决议记录。D1、D2、D4 的问题定位和约束分析正确，尤其是 `0x3000` 对齐要求：原先四槽基址会导致 `pd_ddr_snap_copy` 的 `start_valid=0`，因此必须在改 RTL 前先冻结新的槽地址图。

## D1：四槽地址图

确认采用整体 `+0x1000` 的最小改动方案：

```text
slot0  0x2000_1000 - 0x20C0_0FFF
slot1  0x20C0_1000 - 0x2180_0FFF
slot2  0x2180_1000 - 0x2240_0FFF
slot3  0x2240_1000 - 0x2300_0FFF
```

槽基址与容量均满足 `0x3000` 对齐约束；RTL 不做上取整或地址补偿。

## D2：合法性检查分工与 legacy 路径

确认采用方案 B，但不使用单一全局错误信号直接门控两条路径。

```text
auto 槽快照路径：仅使用 slot_cfg_err；
legacy 自动快照路径：auto_snap_en=1 时禁止发起；
legacy 手动 o_snap_start：始终保留，并只使用 legacy_cfg_err 校验。
```

顶层可向软件汇总报告：

```verilog
snapshot_cfg_err = auto_snap_en ? slot_cfg_err : legacy_cfg_err;
```

但实际启动门控必须按“描述符所有者”分别判断，不能因为 `auto_snap_en=1` 就解除手动路径原有的 `legacy_cfg_err` 保护。

`dm_cp` 只有一个，自动槽快照与 legacy 自动快照不得同时拥有启动权。手动 `o_snap_start` 保留；若与自动槽请求撞车，手动请求优先，自动请求继续保持 pending。

## D3：freeze 事件传递形式

确认采用形式 α，但 `freeze_req` 必须是请求保持信号，而不是一拍脉冲。

顶层检测 `freeze_done` 上升沿，置位 `freeze_req`；`slot_mgr` 在满足全部启动条件后产生单拍 `copy_start`，同时返回 `req_ack`，顶层再清除 `freeze_req`。

```text
freeze_rise
  → freeze_req 置位
  → 等待 ring_write_idle && !copy_busy && !cfg_err && FREE 槽
  → copy_start 单拍 + req_ack
  → freeze_req 清零
```

全槽不可用时，不置 pending，直接 `drop_count++`、置 `slot_full`，并确认/清除该次请求。

Phase 5 第一版定义为“一深度 pending”：若上一次 `freeze_req` 尚未被确认时又来一次新的 `freeze_rise`，新事件不能静默覆盖旧事件，应置 `req_overflow` 或 `drop_busy` 粘滞标志并计数。后续若有连续快照需求，再扩展为请求 FIFO。

## D4：全槽不可用的处理

确认全槽处于 `VALID` 或 `LOCKED` 时立即丢弃当前冻结快照：

- 不等待空槽；
- 不延长冻结窗口；
- 不复用旧槽；
- 下一次快照只接受新的 `freeze_done` 上升沿。

## D5：PS 释放槽的语义

`slot_lock` 定义为 PS 读取所有权保护，采用严格语义：

```text
VALID  --lock-->  LOCKED
LOCKED --release--> FREE
```

- `slot_lock[n]`：W1P/W1S，仅允许对 `VALID` 槽生效；
- `slot_release[n]`：W1P，仅允许对 `LOCKED` 槽生效；
- 对 `FREE`、`RESERVED`、`COPYING`、`VALID` 直接 release：拒绝、状态不变、置粘滞错误；
- `LOCKED` 槽绝不参与自动分配；
- 软件长期不 release 时，槽会持续保留，最终可能触发 full/drop；这是软件可观测的资源耗尽，RTL 不允许偷偷覆盖。

PS 软件标准流程固定为：

```text
查询 VALID → lock → 读取 DDR → release → FREE
```

## 寄存器空间与保留地址

确认 `0x5C` 有意保留：

```text
0x4C - 0x58  控制、状态和计数寄存器
0x5C          保留字，用于 16-byte 对齐
0x60 起       槽描述符寄存器
```

保留地址必须“读回 0、写忽略”，与接口契约保持一致。本次扩展 `pd_ddr_axil.v` 时，应同时修正现有 `default: DEAD_BEEF` 与契约不一致的问题。

## 执行顺序

1. 用本回复与原决议更新接口契约及变更记录。
2. 更新 `pd_ddr_defines.vh`，增加已冻结的四槽地址与容量宏。
3. 由开发者创建 `pd_ddr_slot_mgr.v`，第一步只写模块接口、状态枚举与寄存器定义。
4. 先进行编译检查；通过后再逐段补全请求保持、槽分配、拷贝启动和 AXI-Lite 接口逻辑。

