# TCP API v11：PS 侧 PRPD 统计接口

## 目的

v11 在既有事件归档、FFT、扫描和告警接口之上增加 PRPD 相位统计。它不增加 PL 资源，也不改变 DDR 中的快照格式；PS 从已保留的 64-bit 特征事件包中提取峰值事件的相位、极性和原始 Q8.8 幅度。

## 前置条件

- 固件 `CONFIG` 必须返回 `api=11` 和 `prpd_bins=64`；
- 先执行有限采集，例如 `START 512`，并等 `STATUS` 的 `state=0`；
- `CATALOG` 中必须有保留事件窗口；
- PRPD 查询要求服务 IDLE。采集中会返回错误，而不会读取正在更新的归档。

## 命令

### 汇总

```text
PRPD SUMMARY
```

成功格式：

```text
PRPD seq=[496,512) packets=16 bins=64 ch0=... ch1=... ch2=... ch3=...
```

每个 `chN` 的字段顺序为：

```text
peak_count / positive_count / negative_count / q_abs_mean / q_abs_max / phase_at_max
```

- `q_abs_mean`、`q_abs_max`：未校准的绝对 Q8.8 代码，不能直接标注为 pC；
- `phase_at_max`：PL 产生的原始相位索引；当前工程默认是 12 位，范围 0..4095；
- `packets`：参与汇总的事件包数量，而非峰值数量。

### 相位桶

```text
PRPD BINS channel first_bin [count]
```

参数：

- `channel`：0..3；
- `first_bin`：0..63；
- `count`：可选，1..8，默认 8；
- 第 `b` 个桶统计原始相位 `[b*64, b*64+63]` 的峰值事件数。当前 64 桶由 `phase >> 6` 计算（默认 12 位相位字段的每桶 64 个相位点）。

> 注：源码的相位映射必须与 PL 的 `PD_PH_FIELD_W` 一致。本工程当前为 12 位，故使用 `phase >> 6`。若切换为 10 位相位字段，应同步改为 `phase >> 4`，并更新此文档和上位机解析。

成功格式：

```text
PRPD_BINS seq=[496,512) ch=0 first=0 count=8 values=0,1,0,0,0,0,0,0
```

`values` 按相位桶递增排列。该命令用于上位机分段拉取直方图；完整 64 桶需要 8 次请求。

## 当前限制

1. 统计窗口只覆盖事件归档的 16 个保留包，新的采集会覆盖旧记录；要获得长期趋势，需要由上位机周期性读取并持久化，或后续扩展更大的 PS 环。
2. 统计量基于 PL 已输出的“峰值事件”，不是所有 ADC 点。因此它是 PRPD 事件分布，不是原始波形二维图。
3. 没有标定系数和真实 ADC 量纲时，Q 值只能作为相对原始代码比较。
4. 告警接口与 PRPD 相互独立：`ALERT` 依据频谱扫描特征，`PRPD` 依据峰值事件相位。上位机后续可组合两者形成诊断规则。

## 最小验证

```text
CONFIG
START 512
STATUS
CATALOG
PRPD SUMMARY
PRPD BINS 0 0 8
PRPD BINS 0 8 8
```

应看到 `api=11`、非空 `event_seq`、`PRPD` 汇总和两个格式正确的 `PRPD_BINS` 回复。数值会随 DDS 事件位置改变，不应用某个通道或单一桶的固定数值作为通过条件。
