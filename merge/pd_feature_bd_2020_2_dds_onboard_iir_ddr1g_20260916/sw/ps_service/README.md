# PS-1 模块化采集核心

本目录是对已上板验证的 `pd_acquisition_service.c` 的非阻塞化重构起点。

- `include/pd_hw_map.h`：唯一的 AXI-Lite、DMA 与 DDR 区域契约来源；不得在 TCP/UDP/FFT 文件中复制寄存器偏移。
- `include/pd_acquisition.h`：采集核心的公共 API 与归档元数据。
- `src/pd_acquisition_core.c`：DMA S2MM、事件归档、自动快照、槽锁定/释放与停止排空。

## 当前边界

新增 `tcp/` 后，TCP 前端调用 `pd_acq_init()`、`pd_acq_start()`、`pd_acq_poll()` 和 `pd_acq_request_stop()`，不再通过 `.inc` 直接编译旧采集服务。它不修改 PL，也不覆盖已验证的 TCP V2 工程。

## Vitis 导入规则

在新的 lwIP Echo Server 应用中只导入本目录下的 `src/*.c` 与 `include/*.h`，并保留 Vitis 模板自动生成的 `platform*.c`、`iic_phyreset.c`。不要同时导入旧的 `pd_acquisition_service.c`、`pd_acquisition_tcp_service_main.c` 或 `tcp_service_template/main.c`，否则会发生重复 `main()` 或重复全局变量定义。

## 当前验证要求

`src/main.c` 是 UART 冒烟前端；`tcp/main.c` 是 lwIP TCP 前端。两者都定义 `main()`，因此必须建立两个独立的 Vitis 应用，或每次只导入其中一个。

TCP 应用需要导入：`src/pd_acquisition_core.c`、`src/pd_snapshot_unpack.c`、`src/pd_spectrum.c`、`src/pd_analysis.c`、`include/*.h`、`tcp/pd_tcp_service.c`、`tcp/pd_tcp_service.h` 和 `tcp/main.c`。此外保留 lwIP Echo Server 模板生成的 `platform*.c`、`iic_phyreset.c`。不要导入旧 `pd_acquisition_service.c`、`pd_acquisition_tcp_service_main.c` 或 `tcp_service_template/main.c`。

## PS-2：运行时配置接口

TCP 前端新增以下不改动 PL 的运行时命令：

- `CONFIG`：返回 API 版本、默认采集包数、事件/快照归档容量及 `GET` 单次传输上限。
- `SET LIMIT n`：仅空闲时设置后续无参数 `START` 的默认包数；`n=0` 表示连续采集，配置仅在当前上电会话内保持。
- `SCOPE ON [samples]` / `SCOPE NEXT` / `SCOPE OFF`：实时示波器协议。PS 从 PL 原始 DDR 环形缓冲中读取一个最新窗口（默认 1024 个四通道 48-bit 样本），以 `SCOPE V1` 二进制帧推送给 Qt；不是快照归档下载。仅在采集运行时允许 `NEXT`，`samples` 必须为 256..2048 且是 4 的倍数。
- `START`：不带参数时采用 `SET LIMIT` 保存的默认值；`START n` 始终以显式 `n` 为准。
- `RECOVER`：仅 `FAULT` 状态有效。服务会关闭新触发、等待正在进行的 PL 拷贝结束、逐槽锁定并释放仍未归档的硬件快照、清除槽状态并复位 S2MM DMA。已归档到 PS DDR 的事件与快照不受影响；回复中的 `discarded_slots` 是明确丢弃的硬件槽数。
- `CATALOG`：返回当前 PS DDR 环形归档的保留序号窗口，区间是半开区间 `[first,next)`。
- `EVENT SEQ n`、`SNAP SEQ n`：按单调 sequence 查询归档元数据；若记录已被环形覆盖，会明确返回错误。旧的 `EVENT index`、`SNAP index` 仍保留以兼容已有 PowerShell 下载工具。

配置不写入 Flash，需要明确非易失存储区域与掉电一致性设计后才可实现。`RECOVER` 的丢弃语义已通过命令回复和状态计数公开，不能把它当成无损恢复。

## PS-4：连续采集压力测试

`tools/pd_tcp_soak_test.ps1` 是主机侧验证工具，不属于最终上位机。它以 `START 0` 运行有界时间的连续采集，周期检查服务仍处于 `RUNNING`、事件 sequence 持续推进、`drops=0` 且 `err=` 为空，最后发送 `STOP` 并要求干净回到 `IDLE`。事件与快照归档覆盖是预期行为，不作为失败条件。

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& '.\tools\pd_tcp_soak_test.ps1' -DurationSeconds 120 -StatusPeriodSeconds 5
```

## PS-5：按序号下载

TCP 数据命令同时支持旧的槽下标模式和新的 sequence 模式：

```text
GET EVENT index offset bytes
GET SNAP  index offset bytes
GET EVENT SEQ sequence offset bytes
GET SNAP  SEQ sequence offset bytes
```

后两种模式先由采集核心验证 sequence 仍在 `CATALOG` 的保留窗口内，再解析为当前归档槽下标。因此主机不需要自行处理 `sequence % slot_count`，且覆盖后的旧记录会在发送数据前被拒绝。`sw/tcp_service_template/pd_tcp_download_record.ps1` 的 `-Sequence` 参数可完成分片 CRC32 下载验证。

## PS-6：原始快照解包与板上完整性检查

`include/pd_snapshot_unpack.h` 与 `src/pd_snapshot_unpack.c` 固化 PL 到 PS 的原始波形契约：一个采样时刻占 6 字节，字段为 `{ch3,ch2,ch1,ch0}`，每字段 12-bit offset-binary；四个采样时刻组成一个 24-byte 小端块。`0x800` 是名义零电平。

采集停止且服务为 `IDLE` 后可执行：

```text
ANALYZE SNAP 0
ANALYZE SNAP SEQ 8
```

返回的每个通道三元组为 `min/max/mean`。当前 3,120,000-byte 快照必须报告 `samples=520000`。该命令仅从 PS DDR 归档读数据，不修改归档、滤波或 PL；它是进入 PS 侧窗口、滤波和 FFT 前的格式一致性门槛。

## PS-7：去直流、Hann 窗与 1024 点 FFT

`include/pd_spectrum.h` 与 `src/pd_spectrum.c` 提供四通道顺序定点 FFT：每通道在所选 1024 样本窗口中先计算均值并去直流，再施加周期 Hann 窗，随后运行带每级缩放的 radix-2 前向 FFT。扫描范围为 `bin 1..512`，因此 DC 不可能被误报为主峰。输出的 `amplitude_code` 已按 Hann 的相干增益近似校正，可用于同一采样率、同一配置下的相对幅值比较。

TCP 命令：

```text
FFT SELFTEST
FFT CONFIG
SET FFTFS 26000000
FFT SNAP 0
FFT SNAP 0 4096
FFT SNAP SEQ 8 4096
FFT AUTO SNAP 0
FFT AUTO SNAP SEQ 8
```

`FFT SELFTEST` 生成四路已知主峰（bin `37/83/151/255`）并穿过同一条“打包—解包—去直流—加窗—FFT—主峰”路径；只有返回 `FFT_SELFTEST_PASS` 才进入真实快照分析。`FFT SNAP` 只允许空闲状态执行。默认采样率是当前 DDS 原型的 26 MSPS（bin 分辨率约 25.39 kHz）；将来切换到 65 MSPS 时无需改动 FFT 内核，只需在空闲状态执行 `SET FFTFS 65000000`，此时频率分辨率约为 63.48 kHz。

`FFT AUTO SNAP` 为事件快照选择共同分析窗口：它扫描整段快照，以四通道中相对 `0x800` 偏离最大的原始样本为事件点，选择以该点为中心的 1024 点窗口后再执行同一 FFT 链。回复中 `event=sample/channel/raw_code/delta_from_mid` 明确给出窗口选择依据。这对触发事件不位于快照第一个 1024 点内的情况尤为必要。

## PS-8：可追溯频谱分析记录

`include/pd_analysis.h` 与 `src/pd_analysis.c` 在 PS 内存中维护四项环形分析记录。每项记录绑定输入快照的 `snap_seq`、快照归档槽号、所用采样率、1024 点起点、自动事件窗口依据，以及四个通道的 FFT 结果。它不改动 PL、AXI-Lite 地址、DDR 快照格式或原始数据；快照和分析记录均在后续新记录到来时按各自四槽容量覆盖。

```text
SPECTRUM AUTO SNAP 0
SPECTRUM AUTO SNAP SEQ 8
SPECTRUM SNAP 0 4096
ANALYSIS CATALOG
ANALYSIS 0
ANALYSIS SNAP SEQ 8
```

`SPECTRUM` 运行 FFT 后立即保存记录并返回 `analysis` 序号；`ANALYSIS` 仅查询已保存结果，不会重新读取或重新计算 DDR 数据。这样上位机在一次采集中可以先记录元数据，再按 `snap_seq` 下载同一份原始快照复核。`CLEAR` 在空闲时同时清空采集元数据和分析元数据。实际 Vitis TCP 应用必须把 `pd_analysis.c` 加入 `CMakeLists.txt`；本项目的 `lwip_echo_server5` 已完成该登记。

## PS-9：事件前后五窗口频谱扫描

`pd_analysis.c` 在不改变 FFT 内核的前提下增加一个独立的四槽扫描记录环。`SWEEP AUTO` 先以现有自动事件检测确定事件点，再对以该事件为中心的五个 1024 点窗口执行四通道 FFT；五个窗口的起点相对中心窗口分别为 `-1024`、`-512`、`0`、`+512`、`+1024` 个采样点。靠近快照边界时，起点会安全地截断到合法范围，返回的 `starts=` 字段是最终实际使用值。

```text
SWEEP AUTO SNAP 0
SWEEP AUTO SNAP SEQ 8
SWEEP CATALOG
SWEEP INDEX 0
SWEEP WINDOW 0 0
SWEEP WINDOW 0 2
SWEEP WINDOW 0 4
SWEEP SNAP SEQ 8
```

`SWEEP AUTO` 的回复只返回事件依据和五个起点，因此任何 TCP 客户端都能用一行安全接收。随后通过 `SWEEP WINDOW index ordinal` 单独取得某一个窗口的四通道 `bin/frequency/amplitude/DC`，其中 `ordinal=2` 是事件居中的主窗口，`0/1` 是事件前窗口，`3/4` 是事件后窗口。此功能用于判断频谱成分是否仅在局放脉冲附近增强；它是 PS 侧相对趋势分析，不替代校准后的物理幅值判定。

## PS-10：事件频谱特征摘要

每个 `SWEEP_WINDOW` 现在额外返回 `p<band_power>`：它是该窗口内 `bin 1..512` 的非直流 FFT 功率和。该数值保留 FFT 内部缩放，不能当作标定后的物理功率；但对同一采样率、同一窗口长度和同一软件版本，适合做事件前后相对比较。

```text
FEATURE SWEEP 0
FEATURE SWEEP SNAP SEQ 8
```

`FEATURE` 从已保存的五窗口扫描记录生成一行四通道摘要。每个 `chN` 的五个字段依次是：`pre_band_power/event_band_power/post_band_power/event_delta_permille/recovery_permille`。

- `pre_band_power`：窗口 0、1 的均值；
- `event_band_power`：窗口 2，即事件居中窗口；
- `post_band_power`：窗口 3、4 的均值；
- `event_delta_permille`：事件中心相对事件前基线的变化，单位 ‰；
- `recovery_permille`：事件后相对事件前基线的残差，单位 ‰。

例如 `event_delta_permille=250` 表示事件中心频谱功率比事件前基线高约 25%；负值表示降低。该摘要面向上位机的趋势显示、阈值告警和后续分类算法输入，仍不替代经真实 ADC、传感器和标定链路验证后的物理量判定。

## PS-11：可配置的原型规则判据

PS-11 对已保存的 `SWEEP` 记录计算规则命中位图，不接入 PL 中断、硬件告警输出或任何非易失存储。默认阈值为 `0`，即判据关闭；因此 DDS 原型的测试结果不会在未明确配置时被解释为真实局放告警。

```text
ALERT CONFIG
SET ALERT MASK 15
SET ALERT DELTA 1000
ALERT SWEEP 0
ALERT SWEEP SNAP SEQ 8
```

- `MASK` 是四位通道使能掩码，bit0..bit3 对应 ch0..ch3；`15`（`0xF`）表示四路都参与。
- `DELTA` 是事件中心相对频谱基线的最小增量，单位 ‰；设置为 `0` 会关闭命中判定。
- `ALERT` 返回 `hit_mask`，其中某一位为 1 表示该通道的 `event_delta_permille >= threshold`。

当前 DDS 原型的已验证示例中，ch1 的增量约为 `2866‰`。因此在 `MASK=15`、`DELTA=1000` 时，预期 `hit_mask=0x2`；此结果仅用于验证通道关联和软件规则链，不能作为真实局放报警阈值。后续接入真实 ADC 前必须完成传感器量程、噪声底、重复性、误报率和温度/工频工况下的标定。

`tools/pd_ps11_alert_test.ps1` 将 TCP 建连、`START 512`、自动五窗口扫描、启用规则、关闭规则及断言整合为一次可重复的板端回归测试。它在结束前把阈值恢复为 `0`；当前服务 API 已向后兼容该测试，运行时应使用最新 ELF。

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& '.\tools\pd_ps11_alert_test.ps1'
```

## PS-12：当前快照归档的批量离线分析

`BATCH AUTO` 是一个只在采集服务处于 `IDLE` 时允许执行的 PS 侧离线命令。它遍历当前仍保留在 PS DDR 快照环中的全部快照（最多四份），每份各运行一次既有的五窗口频谱扫描，并按当前 `ALERT MASK/DELTA` 配置汇总规则命中。该过程不读取 PL 寄存器来重触发采集、不改动原始快照数据，也不新增硬件接口。

```text
SET ALERT MASK 15
SET ALERT DELTA 1000
BATCH AUTO
SWEEP SNAP SEQ 6
FEATURE SWEEP SNAP SEQ 6
ALERT SWEEP SNAP SEQ 6
```

成功回复示例：

```text
BATCH snap_seq=[6,10) processed=4 sweep_seq=[20,24) enabled_mask=0xf threshold=1000 hit_union=0x2 hit_records=1
```

- `snap_seq=[first,next)`：本次实际遍历的稳定快照序号窗口；
- `processed`：成功完成扫描的快照数；
- `sweep_seq=[first,next)`：新写入的扫描记录序号窗口；
- `hit_union`：所有本次快照中命中过规则的通道按位或；
- `hit_records`：至少有一个通道命中的快照数。

扫描环和快照环都只有四槽，因此一次完整批量分析正好填满扫描环；后续单次 `SWEEP AUTO` 或下一次 `BATCH AUTO` 会按环形规则覆盖最旧扫描记录。应使用 `SNAP SEQ` 查询而非槽下标来保持可追溯性。`BATCH AUTO` 不是实时告警路径：真实 ADC 的在线告警仍需在后续阶段明确 CPU 预算、触发队列和实时性要求。

板端回归脚本：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& '.\tools\pd_ps12_batch_test.ps1'
```

脚本自行建连、采集、批量分析并验证每份结果都能通过既有 `SWEEP / FEATURE / ALERT` 的按快照序号查询；它在结束前把告警阈值恢复为 `0`。运行前必须下载包含 `BATCH AUTO` 的最新 ELF。

## API v11：PRPD 相位统计

在不改动 PL、DDR 数据格式和既有 TCP 命令契约的前提下，PS 端新增了对保留特征事件环的 PRPD（phase-resolved partial-discharge）汇总。它只读取 `EVENT` 归档中的 64-bit 峰值记录，不读取整份快照，因此适合先完成事件级相位分布、极性和原始幅值统计。

- `PRPD SUMMARY`：汇总当前保留的最多 16 个事件包，分别给出四通道的峰值数、正/负极性数、原始绝对 Q8.8 均值/最大值，以及最大值所在的 PL 相位；
- `PRPD BINS channel first_bin [count]`：读取指定通道的 64 个相位统计桶的一段，默认连续 8 桶，单次最多 8 桶；
- 当前 PL 配置的 12 位相位索引按 `phase >> 6` 映射为 64 桶。它是事件数直方图，不是校准后的 pC/角度值；
- 两条命令均要求服务处于 `IDLE` 且存在保留事件，避免采集写环时读取不稳定归档。

活动工程还需有如下两个新文件，并在 `F:\ps\lwip_echo_server\src\CMakeLists.txt` 中包含源文件：

```cmake
collect (PROJECT_LIB_SOURCES pd_prpd.c)
```

详细字段和命令纪律见 `TCP_API_v11_PRPD说明.md`。更新后下载 `CONFIG api=11` 的 ELF，再执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& '.\tools\pd_ps_final_regression.ps1'
```

## API v10：PS 端交付收口

本次将 PS 端当前硬件条件下可独立完成的能力合并交付，而非继续拆成小阶段：

- `EVENT DETAIL index|SEQ sequence`：解析已归档的 PL 64-bit 特征包，返回每通道峰值包数、最大绝对 Q8.8 码、周期包数、周期内事件总数、`qmax` 和最后周期索引；
- `REPORT`：一行汇总服务状态、事件/快照/分析/扫描的保留序号窗口、告警配置与 DDR/DMA 状态，供未来上位机初始化时做一致性检查；
- `tools/pd_ps_final_regression.ps1`：在一个独立 TCP 会话里覆盖 FFT 自检、采集、目录、事件包解码、批量扫描、特征、告警和统一报告；
- `TCP_API_v10_冻结说明.md`：固定当前命令、字段、二进制 `GET` 纪律和环形覆盖语义，作为未来上位机的接口输入。

当前 Vitis 应用还需要纳入新增的 `pd_event_decode.c`。已经同步的活动工程 `F:\ps\lwip_echo_server\src\CMakeLists.txt` 已添加：

```cmake
collect (PROJECT_LIB_SOURCES pd_event_decode.c)
```

执行完整板端回归：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& '.\tools\pd_ps_final_regression.ps1'
```

如需在测试后清理 PS 元数据（原始 PL DDR 不受影响），追加 `-ClearAtEnd`。运行前必须下载 `CONFIG api=11` 的 ELF。
