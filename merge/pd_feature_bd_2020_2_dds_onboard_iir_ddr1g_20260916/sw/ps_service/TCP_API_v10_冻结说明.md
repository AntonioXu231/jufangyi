# PD 采集 PS TCP API v10（DDS 原型冻结版）

## 目的与边界

本协议是当前 Zynq-7020 裸机 lwIP 服务的 PS 对外契约，服务端口固定为 `TCP 6001`。它面向未来上位机，但本文件不实现上位机界面。所有文本命令和文本回复均以 CRLF 结束；原始数据只通过 `GET` 的“文本头 + 精确长度二进制负载”方式发送。

该版本不改变 PL、AXI-Lite 地址、DMA 包格式、DDR 快照格式或 IIR 配置契约。默认 DDS 验证采样率为 26 MSPS；`SET FFTFS` 仅影响 PS 频谱中由 bin 换算出的频率。

## 稳定对象与覆盖规则

| 对象 | 容量 | 键 | 覆盖规则 |
|---|---:|---|---|
| 特征事件归档 | 16 | `event sequence` | 新事件覆盖最旧事件 |
| 原始快照归档 | 4 | `snapshot sequence` | 新快照覆盖最旧快照 |
| 单窗口分析 | 4 | `analysis sequence` | 新分析覆盖最旧结果 |
| 五窗口扫描 | 4 | `sweep sequence` / 输入 `snapshot sequence` | 新扫描覆盖最旧结果 |

上位机必须优先使用 `... SEQ n` 查询，不得自行猜测 `sequence % slot_count`。先读 `CATALOG` 或 `REPORT` 获得半开区间 `[first,next)`；若序号已离开此区间，服务端必须返回 `ERR`，而不能发送错误归档。

## 核心命令

| 命令 | 条件 | 含义 |
|---|---|---|
| `CONFIG` | 任意 | 返回 API 版本、环容量、默认设置 |
| `REPORT` | 任意 | 一行返回状态、四类序号窗口、告警配置、DMA/DDR 状态 |
| `START [n]` / `STOP` | 按状态机 | 启动或停止有界/连续采集 |
| `STATUS` | 任意 | 采集状态、计数、错误字串 |
| `CATALOG` | 任意 | 事件和快照可查询序号范围 |
| `EVENT SEQ n` / `SNAP SEQ n` | 序号有效 | 返回归档元数据 |
| `EVENT DETAIL SEQ n` | 序号有效 | 解码 PL 64-bit 事件包的每通道摘要 |
| `GET EVENT|SNAP SEQ n offset bytes` | 序号有效 | 分段二进制下载，单次不超过 16 KiB |
| `FFT ...` / `SPECTRUM ...` / `SWEEP ...` | 分析仅限 IDLE | 频谱与五窗口扫描 |
| `FEATURE SWEEP SNAP SEQ n` | 已有扫描 | 返回事件前/中/后带内功率趋势 |
| `SET ALERT MASK n` / `SET ALERT DELTA n` / `ALERT ...` | 设置仅限 IDLE | 原型软件规则判据 |
| `BATCH AUTO` | `IDLE` | 对当前 1–4 份保留快照各创建一次扫描并汇总命中 |
| `CLEAR` | `IDLE` | 只清 PS 元数据，不擦除 PL 原始 DDR 内容 |

## `EVENT DETAIL` 字段

`EVENT_DETAIL` 解码的原始包格式直接对应当前 RTL：峰值包 `type=0` 包含 Q8.8 视在电荷码、12-bit 相位、极性、通道和 PL 局部事件序号；周期包 `type=1` 包含周期索引、通道、周期内事件数和 `qmax`。回复中每个 `chN` 为：

```text
p<峰值包数>/q<最大绝对Q8.8码>/c<周期统计包数>/n<周期内事件总数>/qmax<周期峰值码>/cyc<最后周期索引>
```

这些是数字码，不是已校准的 pC 物理量。真实传感器、ADC、增益与噪声底标定完成前，不得将其直接作为产品级绝缘诊断结论。

## 传输纪律

1. 同一 TCP 连接中，收到 `DATA V2 ... bytes=N crc32=...` 后，客户端必须读取**恰好 N 字节**二进制负载，才可发送下一个文本命令。
2. `GET` 单次最大 16 KiB；3,120,000-byte 快照由客户端按偏移循环下载。
3. 原始快照的下载/解析可在 IDLE 后进行；持续采集时归档会轮转，必须以 sequence 为准处理覆盖错误。
4. `BATCH AUTO`、`SWEEP AUTO`、`SPECTRUM` 只在 IDLE 运行，避免 CPU FFT 与 DMA 采集竞争。

## 当前已完成与保留风险

API v10 已完成轮询采集、DMA 归档、快照归档、分段 CRC 下载、快照解包、1024 点 FFT、五窗口趋势、规则判据、批量离线扫描、事件包摘要和 TCP 回归工具。

未包含的事项：GIC 中断驱动（当前合并中断源未导出独立可验证标识）、真实 ADC/65 MSPS、滤波参数标定、掉电配置持久化、UDP 推送和正式上位机。它们需要 PL/XSA/硬件或产品协议决策，不能仅靠 PS 端代码安全完成。
