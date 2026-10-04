# A15. PS 侧旧版采集服务与调试探针逐行注释

## 本文覆盖范围与与 A13/A14 的分工

本卷（A15）精读 `sw/` 目录下**早期/辅助版**的 PS 侧（ARM Cortex-A9 裸机 C）程序，即当前活跃服务 `sw/ps_service/`（`pd_acquisition_core.c` + `tcp/pd_tcp_service.c`）**之前**或**并存**的下列源码：

- `pd_acquisition_service.c`（整体式采集服务 V1，524 行）
- `pd_capture_service.c`（首个集成级采集服务，301 行）
- `pd_acquisition_tcp_service_main.c`（把 V1 引擎接入 lwIP TCP V2，484 行；其包含的 `pd_acquisition_service.inc` 经 `diff` 确认与 `pd_acquisition_service.c` **逐字节相同**，`tcp_service_template/main.c` 亦与 `pd_acquisition_tcp_service_main.c` 逐字节相同）
- `dma_s2mm_probe.c` / `dma_s2mm_probe_v2.c` / `dma_s2mm_probe_v3.c`（S2MM 包长/流一致性探针系列，401/473/541 行）
- `dma_s2mm_dds_test.c`（最早的 128 字节固定 BTT 接收测试，179 行）
- `pd_feature_dma_s2mm_smoke.c`（特征到 DMA 接收冒烟测试，252 行）
- `pd_filter_apply.c`（IIR 系数安全下发，87 行）
- `pd_filter_axil_smoke.c`（滤波器 AXI-Lite 可达性冒烟，63 行）

并参考工程自带说明：`pd_acquisition_service_使用说明.md`、`pd_capture_service_使用说明.md`、`pd_acquisition_tcp_service_使用说明.md`、`tcp_service_template/README.md`、`tcp_service_template/TCP数据下载V2_协议与使用.md`。

**与 A13/A14 的分工（依据本工程既有目录规划）：**

- A13（假设为 PL 侧/BD 与寄存器契约讲解）负责解释 `pd_feature_0`、`pd_ddr_0`、`pd_filter_0`、`axi_dma_0` 四个 IP 在 Block Design 中的连接、AXI-Lite 寄存器位定义、快照槽与 DDR 环的物理分区。
- A14（假设为当前活跃 PS 服务 `sw/ps_service/` 讲解）负责解释现行常驻采集/下位机协议。
- **A15（本卷）** 只解释上述“旧版/探针”源码本身：逐行说明它们读写了哪些地址、数据流是什么、通过判据是什么、彼此演进关系如何。凡涉及 PL 寄存器位宽、RTL 行为细节之处，本卷只复述源码里写明的宏与注释，**不臆造** RTL 内部实现。

> 约定：本文出现的十六进制地址均来自源码宏或说明文档，已与已知关键事实交叉核对（`pd_feature_0=0x4000_0000`、`pd_ddr_0=0x4001_0000`、`pd_filter_0=0x4002_0000`、`axi_dma_0=0x4040_0000`、`DDR 冻结快照四槽区=0x2000_1000~0x2300_1000` 每槽 12 MiB、`PS 快照归档=0x2400_0000`、`PS 事件归档=0x2700_0000`）。

---

## 一、`dma_s2mm_dds_test.c`（最早的 S2MM 接收测试，179 行）

### 文件定位与演进关系

这是本工程**最早**的 PS 侧 DMA 接收验证程序：仅验证“`pd_feature_0/m_axis` 的 64 位 AXI-Stream 能否经 `axi_dma_0` S2MM 落到 PS DDR”。它的历史作用是把 ILA（集成逻辑分析仪）触发条件架起来，并用串口打印前 16 个流字。它存在两个已被后续版本修正的缺陷（见末尾【观察】），因此**被 `dma_s2mm_probe.c` 取代**，不再作为正式验收用例。

### 硬件地址清单（Q2）

| 宏 / 数值 | 地址 | 对应硬件 |
|---|---|---|
| `XPAR_XAXIDMA_0_BASEADDR` | `0x4040_0000` | `axi_dma_0` AXI-Lite 基址（DMA 基地址） |
| `S2MM_DMACR_OFFSET` | `0x30` | DMA S2MM 控制寄存器（相对 DMA 基址） |
| `S2MM_DMASR_OFFSET` | `0x34` | DMA S2MM 状态寄存器 |
| `S2MM_LENGTH_OFFSET` | `0x58` | DMA S2MM 本次传输字节数寄存器 |
| `XPAR_PS7_DDR_0_BASEADDRESS + 0x01000000` | `0x0110_0000`（推算） | RX 接收缓冲（PS DDR，64 字节对齐外无其它约束） |

`RX_BUFFER_BASE` 由 `PS_DDR_BASE + 0x01000000` 得到，结合探针系列注释推算 `PS_DDR_BASE≈0x0010_0000`，故 RX 缓冲 = `0x0110_0000`。本文件不碰 `pd_ddr_0`、`pd_filter_0`，只与 DMA 交互。

### 数据流向（Q3）

`pd_feature_0/m_axis`（PL 64 位 AXI-Stream）→ `axi_dma_0` S2MM（Simple 模式）→ PS DDR 经 HP0 → `0x0110_0000` 缓冲 → 程序读出 `volatile u64*` 并打印。没有归档、没有快照，纯“接住并打印”。

### 通过判据（Q4）

没有显式 PASS 字符串。判定“正常”的隐含条件是：
- 循环能持续完成 `XAxiDma_SimpleTransfer` 且 `i != POLL_LIMIT`（未超时）；
- `DMASR` 无 `DMASR_ERROR_MASK`、无 `DMASR_HALTED`、有 `DMASR_IDLE`；
- 否则打印 `FAIL: transfer=... DMACR=... DMASR=... LENGTH=... polls=...` 并 `return -1`（第 161–164 行）。

### 逐行注释

```
1   /* 文件头注释
→ 说明：这是 Zynq-7000 裸机下、针对板载 DDS 构建的最小 S2MM 接收测试。注释列出了已核实的硬件事实（DMA 基址、S2MM 使能、SG 关闭、DDR HP0 窗口），并强调本程序只启动 PS、配置 DMA 寄存器，ILA 由 Vivado 侧事先布好。
```

```
15  #include "xparameters.h"
→ Xilinx 自动生成的板级参数头：提供 XPAR_* 基地址宏与设备 ID，是后面所有地址宏的来源。若 XSA 未导出对应 IP，这些宏就不存在，编译会失败。
16  #include "xaxidma.h"
→ Xilinx AXI DMA 驱动头：提供 XAxiDma、XAxiDma_LookupConfig、XAxiDma_CfgInitialize、XAxiDma_SimpleTransfer、XAxiDma_Busy、XAxiDma_Reset 等 API。
17  #include "xil_cache.h"
→ 提供 Xil_DCacheFlushRange / InvalidateRange：Cortex-A9 有数据 cache，DMA 写内存后 CPU 可能读到旧 cache 行，必须 invalidate；DMA 前也要 flush 防止旧脏行污染。
18  #include "xil_io.h"
→ 提供 Xil_In32 / Xil_Out32：裸机下对绝对物理地址做 32 位读写的底层函数（本质带内存屏障的 MMIO 读）。
19  #include "xil_printf.h"
→ 裸机串口打印（走 PS UART0，约 115200-8-N-1），比标准 printf 更轻、不依赖 C 库堆。
20  #include "sleep.h"
→ 提供 usleep：本文件用它在每次传输之间插入 1 ms 间隙（第 177 行），故意制造低 TREADY 窗口便于 ILA 观察。
```

```
27  #ifdef SDT
28  #ifndef XPAR_XAXIDMA_0_BASEADDR
29  #error "AXI DMA base-address macro is absent..."
→ 分支说明：Vitis 2024 默认用 System Device Tree（SDT），DMA 驱动按“基地址”查实例；旧版（2020.2）按 DeviceId 查。这里两分支都给出，保证新/旧 BSP 都能编译。注意它与后面 probe 系列的分支写法略有不同（这里用 #ifdef SDT，probe 系列直接用宏是否存在判断）。
```

```
39  #if defined(XPAR_PS7_DDR_0_BASEADDRESS)
40  #define PS_DDR_BASE  XPAR_PS7_DDR_0_BASEADDRESS
...（略，同后续文件的 PS DDR 基址探测）
```

```
47  #define RX_BUFFER_BASE   (PS_DDR_BASE + 0x01000000U)
→ RX 缓冲放在 PS DDR 偏移 16 MiB 处。规避代码/栈区，也远离后面采集服务用到的原始环（0x1000_2000 起）。
48  #define RX_WORDS         16U
49  #define RX_BYTES         (RX_WORDS * 8U)
→ 本程序只申请/检查 16 个 64 位字 = 128 字节的接收缓冲。这是它最关键的“硬编码”缺陷来源（见【观察】1）。
50  #define POLL_LIMIT       200000000U
→ 每次传输的轮询上限 2 亿次。注释在后续文件解释：一次 DMASR AXI-Lite 读约几十个核心周期，2 亿次约 20 秒；超时即当作“源不发射”而非“DMA 错”。
51  #define REPORT_PERIOD    1024U
52  #define DMA_PERIOD_US    1000U
→ 每 1024 次传输打印一次进度；每次传输后 usleep(1000) = 1 ms 间隙。
```

```
54  #define S2MM_DMACR_OFFSET 0x30U
55  #define S2MM_DMASR_OFFSET 0x34U
56  #define S2MM_LENGTH_OFFSET 0x58U
→ PG021 规定的 S2MM 寄存器偏移。注意：DMA 是“MM2S + S2MM”双通道，这里只用 S2MM（Device→DMA，即 PL→PS）。
57  #define DMASR_HALTED      0x00000001U   /* bit0 */
58  #define DMASR_IDLE        0x00000002U   /* bit1 */
59  #define DMASR_DMA_INT_ERR 0x00000010U   /* bit4 */
60  #define DMASR_DMA_SLV_ERR 0x00000020U   /* bit5 */
61  #define DMASR_DMA_DEC_ERR 0x00000040U   /* bit6 */
63  #define DMASR_ERR_IRQ     0x00004000U   /* bit14 */
64  #define DMASR_ERROR_MASK  (... | ...)
→ 这些都是 DMASR 状态位。关键概念：Halted（bit0）表示通道被 RS 清 0 或出错停机；Idle（bit1）表示通道空闲、可提交下一笔；错误位集合用于快速判错。
→ 若只靠 XAxiDma_Busy()：其实现仅是“(SR & IDLE) ? FALSE : TRUE”，即只要不是 Idle 就返回 Busy，Halted 时也会永远返回 TRUE——这正是后续 probe 系列弃用 XAxiDma_Busy 改用直接读 DMASR 的原因。
```

```
67  static XAxiDma AxiDma;
→ 驱动句柄（全局静态），保存 DMA 配置与运行时状态。Simple 模式下它基本只保存寄存器基址与模式标志。
```

```
69  static void print_stream_word(u32 index, u64 event_word)
→ 把 64 位流字拆成 hi/lo，按 type（高 8 位）分别解释为“峰值事件（0x00）”或“周期统计（0x01）”。
```

```
76  if (type == 0x00U) {
77  /* Peak event: [63:56]type [55:40]q [39:30]phase [29]pol [28:27]ch [26:0]seq. */
→ 【观察】2（缺陷）：这里用的是 10-bit 相位图（phase=((hi&0xff)<<2)|((lo>>30)&0x3)、pol=bit29、ch=bit28:27、seq=bit26:0）。但本构建实际 PD_PH_FIELD_W=12（见 pd_defines.vh 与 probe 系列），正确布局应为 phase[39:28]、pol[27]、ch[26:25]、seq[24:0]。本文件的相位/极性/通道/序号全部错位 2 位。probe 系列显式修正了这一点。
```

```
100 int main(void)
→ 程序入口。裸机 main 不退出的程序通常用 for(;;) 或停在 WFI。本程序是“无限循环接收直到 FAIL”。
```

```
110 cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
→ 用基地址（SDT）或 DeviceId（旧 BSP）查 DMA 配置。返回 NULL 即平台未含该 DMA，直接报错退出。
116 XAxiDma_CfgInitialize(&AxiDma, cfg);
→ 把配置写进驱动句柄，使之知道寄存器基址、是否 SG 等。
121 if (XAxiDma_HasSg(&AxiDma)) { ... return -1; }
→ 安全守卫：本程序依赖 Simple 模式。若硬件被配成 Scatter-Gather，SimpleTransfer 行为不对，立即退出。
126 XAxiDma_IntrDisable(...);
→ 关闭 S2MM 中断。本程序全程轮询，不用中断，关掉避免误触发。
127 XAxiDma_Reset(&AxiDma);
128 while (!XAxiDma_ResetIsDone(&AxiDma)) { }
→ 复位 S2MM 并等待完成。复位后才能稳定提交首笔传输。
```

```
132 s2mm_cr = Xil_In32(XPAR_XAXIDMA_0_BASEADDR + S2MM_DMACR_OFFSET);
133 s2mm_sr = Xil_In32(XPAR_XAXIDMA_0_BASEADDR + S2MM_DMASR_OFFSET);
→ 打印复位后的 DMACR/DMASR。注意这里直接用宏字面量而非封装函数 dma_rd，与后续 probe 系列风格不同但等价。
```

```
137 for (transfer = 0U; ; ++transfer) {
→ 无限循环：每轮接收一包 AXI-Stream（直到 TLAST）。
139 Xil_DCacheFlushRange(RX_BUFFER_BASE, RX_BYTES);
140 Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_BYTES);
→ 关键 cache 维护：flush 把可能残留的脏 cache 行写回（这里其实缓冲是 DMA 写目标，flush 主要是清掉“旧内容”以防 invalidate 又把旧行当有效）；invalidate 让后续 CPU 读强制从 DDR 取最新。若省略，CPU 可能读到上一次残留的旧 64 位字，误以为是新数据。
141 XAxiDma_IntrAckIrq(...);
→ 清 DMA 中断挂起位（即便中断关了，状态位也需 ack 才能继续）。
143 XAxiDma_SimpleTransfer(&AxiDma, RX_BUFFER_BASE, RX_BYTES, XAXIDMA_DEVICE_TO_DMA);
→ 提交一笔 S2MM 传输：告诉 DMA“把 PL 流写进 0x0110_0000，最多 RX_BYTES=128 字节”。BTT（Bytes To Transfer）=128。
```

```
150 for (i = 0; i < POLL_LIMIT; ++i) {
151     if (!XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA)) break;
152 }
→ 【观察】3（缺陷）：这里只用 XAxiDma_Busy() 判完成。如上所述，Busy 在 Halted 时也会返回 TRUE，所以一旦出错停机，会一直自旋到 2 亿次才超时，把“真正的 DMA 错误”淹没在漫长等待里。后续 probe 系列改成读 DMASR 的 Idle/Halted/Error 立即跳出。
```

```
159 if (i == POLL_LIMIT || (s2mm_sr & DMASR_ERROR_MASK) ||
160     (s2mm_sr & DMASR_HALTED) || !(s2mm_sr & DMASR_IDLE)) {
161     xil_printf("FAIL: ...");
→ 通过判据反向表达：超时 / 有错误位 / 已 Halted / 非 Idle，都判 FAIL。唯一“成功”是 i<POL_LIMIT 且 (无错误 & 未停 & Idle)。
```

```
166 Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_BYTES);
→ 传输完成后再次 invalidate，确保 CPU 读到的就是 DMA 实际写的数据（前面那次 invalidate 在提交前，提交后再 inval 一次更稳妥）。
167 if (transfer == 0U) { ... print_stream_word ... }
→ 只在第 0 包打印 16 个字原文。后续包只周期性报告“还在跑”。
173 if ((transfer % REPORT_PERIOD) == 0U) { ... }
177 usleep(DMA_PERIOD_US);
→ 故意留 1 ms 低 TREADY 间隙，方便 ILA 抓“两个包之间”的时序。
```

### 易错点小结（`dma_s2mm_dds_test.c`）

1. **BTT 硬编码 128 字节**：与 AXI DMA 硬件行为冲突。PG021 规定 S2MM_LENGTH 必须 ≥ 最大预期包长，否则“小于接收包”会导致未定义行为；本构建的包可长达数千字节（probe 实测最大 8224 B）。这正是被 `dma_s2mm_probe.c` 取代的根因，且当时实测报 `FAIL: ... LENGTH=128` 与 `DMASR=00005011`（bit4 DMAIntErr 置位）。
2. **峰值事件字段按 10-bit 相位图解析**：与当前 RTL 的 `PD_PH_FIELD_W=12` 不符，打印的 phase/pol/ch/seq 全错位。功能验证无碍（仍能区分 type），但数值不可信。
3. **依赖 `XAxiDma_Busy()` 判完成**：出错停机时永久自旋至 2 亿次，无法及时暴露真实错误。
4. **无归档、无快照**：仅用于“把流接住并打印”，不能当作采集服务的验收依据。

---

## 二、`dma_s2mm_probe.c`（S2MM 包长探针 v1，401 行）

### 文件定位与演进关系

取代 `dma_s2mm_dds_test.c`。目标是**实测** S2MM 数据包长度分布，并回答一个关键问题：**S2MM_LENGTH 读回的是“已写字节数”还是“剩余字节数”**。它把 BTT 提升到最大合法值 65528，去掉 1 ms 间隙（连续排空），用非 cache 映射免去 cache 维护，且每次完成都读回 LENGTH。实测结果（记录在 `HANDOFF_2026-09-15_DMA_ILA.md`）：约 3 万包零错误，LENGTH 最小 8（=1 beat），证实寄存器是“已写字节数”。

### 硬件地址清单（Q2）

与 `dma_s2mm_dds_test.c` 相同：`axi_dma_0` 基址 `0x4040_0000`，S2MM 偏移 `0x30/0x34/0x58`；RX 缓冲 `0x0110_0000`。本文件额外新增状态位宏：
- `DMASR_IOC_IRQ 0x00001000`（bit12，IOC 中断挂起，用于推断“缓冲在 TLAST 前填满”）
- `DMASR_ERR_IRQ 0x00004000`（bit14，错误中断挂起，并入 ERROR_MASK）

### 数据流向（Q3）

同 dds_test：PL AXI-Stream → S2MM → 0x0110_0000。本文件不复制、不归档，只在 PS 内统计直方图并（可选）打印原文。

### 通过判据（Q4）

本程序**没有 PASS 字符串**，它是“诊断探针”而非验收用例。它的“正常”是无限循环持续接收、零错误位；一旦出错它会打印：
```
FAIL: transfer=... polls=... DMACR=... DMASR=... LENGTH=...
```
并逐位解读错误原因，然后 `return -1` 终止（**不复位、不重试**，以便保留现场证据）。另有一类 `TIMEOUT: ...`：明确区分“源不发射 TLAST”与“真正 DMA 错误”。

### 逐行注释（仅标注与 dds_test 的差异/新增要点）

```
62  /* 文件头长注释
→ 详细解释为什么上一代 128 字节 BTT 会 FAIL（DMASR bit4 DMAIntErr：包大于 LENGTH 或写内存出错），并列出 6 条与 dds_test 的区别。这是理解整个 probe 系列的“设计说明”，务必先读。
```

```
68  #include "xil_mmu.h"
→ 新增：提供 Xil_SetTlbAttributes，用于把 RX 缓冲整段映射为 non-cacheable，从而彻底免去每笔的 flush/invalidate（见第 284 行）。
```

```
75  #if defined(XPAR_XAXIDMA_0_BASEADDR) ... #elif defined(XPAR_AXIDMA_0_BASEADDR) ...
→ 这里用“宏是否存在”判断 SDT/legacy，比 dds_test 的 #ifdef SDT 更稳妥（不依赖 SDT 宏是否定义）。
```

```
102 #define S2MM_MAX_BTT      65528U
→ 最大合法 Simple 模式 BTT：c_sg_length_width=16 → MaxTransferLen=65535，向下取到 8 的倍数（64 位流按 8 字节对齐）。再大 XAxiDma_SimpleTransfer 会返回 XST_INVALID_PARAM。
103 #define BTT_WORDS         (S2MM_MAX_BTT / 8U)   /* 8191 */
→ 65528/8 = 8191 个 64 位 beat。
```

```
114 #define POLL_LIMIT        20000000U
→ 由 dds_test 的 2 亿降到 2000 万。注释解释：一次 AXI-Lite 读 DMASR 约几十核心周期，2000 万次约 1 秒，远多于一个 50 Hz 工频周期；且只“界定等待”，超时被报 TIMEOUT 而非 DMA 错误，避免把“源静默”误判成“调查中的失败”。
```

```
125 #define RX_NONCACHEABLE   1
→ 开关：=1 时把 RX 缓冲映射 non-cacheable，跳过 cache 维护；=0 退回 flush/invalidate 路径（第 317–321 行的 #if !RX_NONCACHEABLE 分支）。
```

```
132 #define DMASR_DMA_INT_ERR  0x00000010U  /* bit4 */
...（SLV_ERR bit5, DEC_ERR bit6, IOC_IRQ bit12, ERR_IRQ bit14）
139 #define DMASR_ERROR_MASK   (INT|SLV|DEC|ERR_IRQ)
→ 注意 v1 的 ERROR_MASK 含 ERR_IRQ（bit14），不含 IOC_IRQ。含义：把“错误中断挂起”也当作错误信号。
```

```
142 /* Peak-event field map 段
→ 修正 dds_test 的 10-bit 缺陷：本构建 PD_PH_FIELD_W=12，故 phase[39:28]、pol[27]、ch[26:25]、seq[24:0]。用编译期宏块保证“改一处即全局一致”，并写清 10/12 两套布局。这是 v1 相对 dds_test 的正确化修复（虽 v1 的主要目标不是字段解析，但仍顺手修正）。
```

```
171 #define NBUCKETS 14
173 static const u32 bucket_hi[NBUCKETS] = { 8,16,32,...,65528 };
177 static u32 hist[NBUCKETS];
178 static u32 hist_over;   /* >BTT：无错误时不可能，作陷阱计数器 */
179 static u32 hist_zero;   /* 一个 TLAST 但无负载，需排查 */
→ 长度直方图：因为 beat=8 字节，桶按字节上界分档。hist_over 用于捕获“理论上不可能的超大包”，hist_zero 捕获“空包”。二者都属异常信号。
```

```
282 #if RX_NONCACHEABLE
284     Xil_SetTlbAttributes((INTPTR)RX_BUFFER_BASE, NORM_NONCACHE);
→ 关键：0x01100000 是 1 MB 对齐的，可以整段改 TLB 属性为 non-cacheable（NORM_NONCACHE）。此后 CPU 对该地址的读写直穿 DDR，无需 flush/invalidate。若地址不对齐，Xil_SetTlbAttributes 只能按 1 MB 页改，会误伤邻区——这里对齐是硬前提。
```

```
316 for (transfer = 0U; ; ++transfer) {
317 #if !RX_NONCACHEABLE
319     Xil_DCacheFlushRange(...); 320 Xil_DCacheInvalidateRange(...);
→ 仅当 RX_NONCACHEABLE=0 才做 cache 维护（本默认 1，故这俩被编译掉）。
```

```
322 XAxiDma_IntrAckIrq(...);
324 XAxiDma_SimpleTransfer(&AxiDma, RX_BUFFER_BASE, S2MM_MAX_BTT, ...);
→ 提交 BTT=65528 的传输，TREADY 持续拉高以连续排空事件 FIFO。
```

```
336 for (polls = 0U; polls < POLL_LIMIT; ++polls) {
337     s2mm_sr = dma_rd(S2MM_DMASR_OFFSET);
338     if ((s2mm_sr & DMASR_IDLE) || (s2mm_sr & DMASR_HALTED) ||
339         (s2mm_sr & DMASR_ERROR_MASK)) break;
→ 与 dds_test 的本质区别：直接读 DMASR，只要“Idle 或 Halted 或 任一错误位”就跳出，不依赖 XAxiDma_Busy。这样 Halted/错误能立即被发现，而不是自旋一整轮。
```

```
347 actual = s2mm_len;   /* S2MM_LENGTH 读回即“实际写字节数” */
→ 文件头“How to read the result”专门讨论：若单 beat 读回 8 → 是“已写字节数”；若读回 (BTT-8)=65520 → 则是“剩余计数”，需换算 bytes=BTT-LENGTH。v1 实测为前者。
```

```
349 if ((s2mm_sr & DMASR_ERROR_MASK) || (s2mm_sr & DMASR_HALTED)) {
350     xil_printf("FAIL: ...");
351~357 逐位打印 Halted/IntErr/SlvErr/DecErr/Err_Irq/IOC_Irq 含义
→ 出错即锁存证据、打印、return -1。明确“不复位、不重试”，让首次失败现场保留给调试者。
```

```
366 if ((polls == POLL_LIMIT) && !(s2mm_sr & DMASR_IDLE)) {
367     xil_printf("TIMEOUT: ...");
→ 超时而 DMASR 仍非 Idle：说明源根本没有发射 TLAST（或总线卡住），这不是“调查中的 DMA 错误”，单独提示，避免误导。
```

```
376 /* 成功完成：LENGTH 即包长 */
377 if ((transfer < DUMP_FIRST_N) || ((actual > DUMP_LONG_BYTES) && ...)) {
382     xil_printf("transfer=%u bytes=%u beats=%u polls=%u DMASR=%08x\r\n");
384     dump_words(actual);
→ 前 4 包或“长包（>8 字节）且未超长包上限”时打印原文。DUMP_FIRST_N=4、DUMP_LONG_BYTES=8、DUMP_LONG_MAX=8。
```

```
387~394 更新 min/max/sum、hist_add(actual)、pkt_count++
396 if ((transfer % REPORT_PERIOD) == 0U) { print_histogram(); }
→ 每 128 包打印一次统计。REPORT_PERIOD=128。
```

### 易错点小结（`dma_s2mm_probe.c`）

1. **non-cacheable 依赖 1 MB 对齐**：`RX_BUFFER_BASE=0x0110_0000` 确为 1 MB 对齐，前提成立；若有人把缓冲挪到非对齐地址，TLB 改属性会误伤邻区。
2. **BTT 不能超 65535**：65528 是安全上界；再大驱动直接拒收。
3. **LENGTH 语义需实测确认**：代码假设“读回=已写字节数”，文件头给出反例换算公式，属于“先验证再信任”的工程纪律。
4. **v1 是无限循环**：会一直占满 CPU 与总线（见 v3 的运维教训）。

---

## 三、`dma_s2mm_probe_v2.c`（流一致性探针 v2，473 行）

### 文件定位与演进关系

v2 = v1 的代码 + **流一致性核算**（`account_packet`）。v1 只测“包长分布”，v2 要解决“包长分布意味着什么”的二选一解释：
- (A) 速率驱动：四通道共享 50 Hz 同步，每工频周期形成 4 个包（3 个 1-beat + 1 个跨整周期的），包长随放电率线性增长；
- (B) 缓冲驱动：大包 = 4×256 FIFO 饱和回吐，天花板由 FIFO 深度决定。

v2 用流内自带信息（cycle 包的 `cycle_idx`、`n`）核算 `peak ≈ cycN` 是否成立来判别，并要求 `lastnotcyc == 0`、`multicyc == 0`（TLAST 契约：每包恰一个 type=0x01 且为末字）。

### 硬件地址清单（Q2）

与 v1 完全相同（仅 DMA）。无新增寄存器。

### 数据流向（Q3）

同 v1：PL → S2MM → 0x0110_0000，仅 PS 内统计，不落地。

### 通过判据（Q4）

仍无 PASS 字符串（诊断性质）。它要求这些“契约校验计数器”保持为 0：
- `acct_last_not_cycle == 0`（每个包的最后一个字必须是 type=0x01）
- `acct_multi_cyc == 0`（每个包恰有一个 type=0x01）
- `acct_other_words == 0`（不允许出现 type 既非 0x00 也非 0x01 的字）
并期望 `pktsPerCycle_x100 ≈ 400`（每工频周期约 4 包）。若 `peak >> cycN` 则解释 (B) 成立（天花板是 FIFO 深度）；若 `peak ≈ cycN` 则 (A) 成立（BTT=65528 只是余量，非结构保证）。

### 逐行注释（仅标注新增/变化点）

```
1~88 文件头注释
→ 复述 v1 结论，并给出 v2 的判别方法与“How to read the result”解读公式。注意它再次强调 2026-09-15 修正的“10-bit→12-bit”字段缺陷，要求宏与 RTL 同步改。
```

```
173 /* stream accounting 段 */
174 static u32 acct_peak_words;   /* type 0x00 计数 */
175 static u32 acct_cyc_words;    /* type 0x01 计数 */
176 static u32 acct_cyc_n_sum;    /* 所有 cycle 包 n 之和 = RTL 自报事件总数 */
177 static u32 acct_other_words;   /* 未知 type 计数（契约外） */
178 static u32 acct_last_not_cycle;/* 末字非 cycle 的包数（须为 0） */
179 static u32 acct_multi_cyc;     /* 含多于一个 cycle 字的包数（须为 0） */
180 static u32 acct_cyc_idx_min/max/seen; /* cycle_idx 极值，用于算经历的周期数 */
183 static u32 polls_min/max;       /* 最短/最长等待轮数 */
185 static u32 bytes_max_polls;     /* 最长包对应的等待轮数 */
→ 这些静态全局保存跨包的累计量，是 v2 相对 v1 唯一新增的状态。
```

```
261 static void account_packet(u32 bytes, u32 polls)
→ 每包都走一遍：遍历 DMA 实际写的每个 beat，累加 peak/cyc/other，记录末类型，统计本包 cycle 字个数；更新 cycle_idx 极值（用模 2^24 比较以容忍回绕）；更新 polls 极值。
```

```
300 if ((words != 0U) && (last_type != 0x01U)) acct_last_not_cycle++;
→ TLAST 契约校验：非空包且最后一个字不是 cycle 统计字 → 计数+1。正常必须恒为 0。
303 if (cyc_here > 1U) acct_multi_cyc++;
→ 每包应恰含一个 cycle 字（它是 TLAST 的来源）。出现多个 → 契约模型错。
```

```
311 static void print_report(void)
→ 输出 packets/min/max/avg、直方图、acct 四项（peak/cyc/cycN/other/lastnotcyc/multicyc）、cycles、pktsPerCycle_x100、evtsPerCycle、polls 极值。
314 u32 cycles = acct_cyc_idx_max - acct_cyc_idx_min;
→ 经历工频周期数 = cycle_idx 极差。注意 cycle_idx 在 RTL 是每 50 Hz 自增的免费计数器。
329 pktsPerCycle_x100 = pkt_count*100/cycles;
→ 每周期包数×100（定点）。预期 ≈400 → 即每周期 4 包，支撑解释 (A)。
```

```
340 int main(void)
→ 主循环与 v1 几乎一致（提交 65528 BTT、直接读 DMASR 跳出、出错锁存 FAIL、超时 TIMEOUT），唯一区别是在成功完成后额外调用 `account_packet(actual, polls)`（第 444 行）并在定期报告里改调 `print_report()`。
```

### 易错点小结（`dma_s2mm_probe_v2.c`）

1. **继承 v1 的全部前提**（非 cacheable 对齐、BTT 上限、LENGTH 语义）。
2. **`account_packet` 每包全遍历**：对最大 8224 B 包要遍历 ~1028 个 beat，纯 PS 软件统计，会消耗 CPU；但它是“测量”而非“在线处理”，可接受。
3. **`acct_cyc_idx_max` 比较用 `idx - max < 0x800000` 容忍回绕**：因为 cycle_idx 是 24 位自由计数，跨 2^24 回绕时若简单 `>` 会误判；这种模比较是正确写法。
4. **v2 仍是无限循环**：见 v3 的运维教训——连续满负载会让后续“再次下载”调试会话把板子误判为不响应。

---

## 四、`dma_s2mm_probe_v3.c`（流一致性探针 v3，541 行）

### 文件定位与演进关系

v3 = v2 的代码 + **有界运行**（`PKT_LIMIT`）+ **停后 park（WFI）**。v2 的副作用是：一旦 Vitis 释放 CPU（“con”），板子就以满负载无限运行，导致下次 `rst -processor` 后出现 `Memory write error at 0x100000` 和 `DAP status 0xF0000021` 的“粘连错误回声”（重试无用，因为真正首错是前一次失败的总线事务留下的板载状态）。v3 收尾打印最终报告并 `park_forever()`（WFI，DMA 不再 re-arm），让总线静默、下次下载从干净目标开始。

**结论：v1/v2/v3 是同一探针的三代；v3 是“可在板子上安全反复运行”的最终形态。**

### 硬件地址清单（Q2）

同 v1/v2，仅 DMA。

### 数据流向（Q3）

同 v1/v2。

### 通过判据（Q4）

仍无 PASS 字符串。正常终止打印：
```
*** RUN COMPLETE: <pkt_count> packets, <bytes_sum> bytes ***
PARKED: S2MM no longer re-armed, CPU in WFI.
Target is quiet; safe to halt and re-download.
```
相反，出错仍是 `FAIL: ...` 锁存现场，超时仍是 `TIMEOUT: ...`。`PKT_LIMIT=20000`，约 100 秒（按 ~200 包/秒）足够直方图与核算使用。

### 逐行注释（仅标注新增点）

```
128 #define PKT_LIMIT         20000U
→ 有界运行上限：收满 2 万包即停。注释算过约 100 s，远多于统计所需。
```

```
380 #if defined(__arm__) || defined(__aarch64__)
381 #define CPU_PARK_STEP()   __asm__ __volatile__("wfi")
382 #else
383 #define CPU_PARK_STEP()   do { } while (0)
→ 可移植的“停机指令”封装。wfi（Wait For Interrupt）让 Cortex-A9 停发总线事务、进入低功耗等待，使目标静默。非 ARM 构建（如主机模拟）则空操作。
```

```
386 static void park_forever(void) { for (;;) CPU_PARK_STEP(); }
→ 永久停机。注意它**不** re-arm S2MM，故 PS 不再发起任何总线事务；PL 事件 FIFO 会满并反压 feature core，这是预期且无害的（无在途事务）。
```

```
439 for (transfer = 0U; ; ++transfer) {
→ 主循环结构与 v2 相同，直到第 532 行新增的终止检查：
532 if (pkt_count >= PKT_LIMIT) {
533     xil_printf("*** RUN COMPLETE: ... ***");
535     print_report();
537     xil_printf("Target is quiet; safe to halt and re-download.\r\n");
538     park_forever();
→ 收满即打印最终报告并 park，不再无限循环。这是 v3 的全部增量价值。
```

### 易错点小结（`dma_s2mm_probe_v3.c`）

1. **务必在重新下载前 halt/power-cycle 板子**：这是 v3 文件头反复强调的运维铁律。连续运行的探针与“板子不响应”在调试器侧不可区分。
2. 继承 v1/v2 全部前提。
3. `park_forever` 后 PL FIFO 反压属预期，不影响下次下载。

---

## 五、`pd_feature_dma_s2mm_smoke.c`（特征→DMA 接收冒烟，252 行）

### 文件定位与演进关系

在“自动快照测试”之后，专门证明 **`pd_feature_0/m_axis` → `axi_dma_0` S2MM → PS DDR** 这条 PS 接收通路。它是**有界**（收 128 包）且**收尾 park** 的验收冒烟，通过打印 `FEATURE_DMA_S2MM_PASS`。它比 probe 系列更“验收导向”：额外强制校验“每包以 type=0x01 结尾”（TLAST 来源），并跟踪 peak/cycle 字数。它可被 `pd_capture_service.c` 的失败处理引用为“先回退跑这个确认 DMA 基础通路”。

### 硬件地址清单（Q2）

仅 DMA：`axi_dma_0=0x4040_0000`，S2MM 偏移 `0x30/0x34/0x58`。RX 缓冲 `0x0110_0000`。**不碰** pd_ddr/pd_filter（区别于采集服务）。

### 数据流向（Q3）

`pd_feature_0/m_axis`（64 位 AXI-Stream）→ S2MM（Simple）→ HP0 → PS DDR `0x0110_0000` → 程序读出校验。不落归档。

### 通过判据（Q4）

成功末行（第 243–246 行）：
```
FEATURE_DMA_S2MM_PASS packets=128 peak_words=... cycle_words=... bytes_min=... bytes_max=... bytes_avg=...
```
失败则打印 `FEATURE_DMA_S2MM_FAIL: <reason>` 并 park（WFI）。具体失败分支：LookupConfig 失败、CfgInitialize 失败、是 SG 模式、SimpleTransfer 失败、S2MM 超时/Halted/AXI 错、LENGTH 非法（0/超 BTT/非 8 对齐）、`inspect_packet` 契约失败（末字非 cycle）。

### 逐行注释（要点）

```
64 #define S2MM_MAX_BTT         65528U
65 #define PACKET_LIMIT         128U
66 #define POLL_LIMIT           20000000U
67 #define REPORT_PERIOD        16U
→ 有界 128 包、BTT 65528、轮询上限 2000 万（与 probe 一致）。REPORT_PERIOD=16 比 probe 更密。
```

```
85 #define EV_TYPE_PEAK         0x00U
86 #define EV_TYPE_CYCLE        0x01U
→ 用具名宏取代裸 0x00/0x01，可读性更好（probe 系列仍用字面量）。
```

```
95 static void park_after_failure(const char *reason)
→ 统一失败处理：打印 FAIL + DMACR/DMASR/LENGTH，然后 `for(;;) __asm__ volatile("wfi")` 停机。与 pd_filter 系列一致风格。
```

```
107 static void print_event_word(u32 index, u64 word)
→ 与 probe 相同的 12-bit 字段解析（phase[39:28]、pol[27]、ch[26:25]、q 在 hi[23:8] 的 s16、cycle 的 idx/ch/n/qmax）。
```

```
135 static int inspect_packet(u32 packet_index, u32 bytes, u32 *peak_words, u32 *cycle_words)
→ 核心契约校验：
143 if ((bytes==0U)||(bytes>S2MM_MAX_BTT)||((bytes&7U)!=0U)) return XST_FAILURE;
→ 字节数必须非零、不超 BTT、8 字节对齐（beat 边界）。
146~152 遍历所有 beat，按 type 累加 peak/cycle 计数；
154 last_type = rx[words-1] >> 56;
155 if (last_type != EV_TYPE_CYCLE) return XST_FAILURE;
→ 契约：每包最后一个字必须是 cycle 统计字（它就是 TLAST 的 RTL 来源）。违反即失败。
158~166 对前 2 包或每 16 包打印前 8 个字详情（与 probe 的 dump 思路一致，但更克制）。
```

```
170 int main(void)
→ 初始化（Lookup/CfgInit/HasSg 守卫/IntrDisable/Reset）与 probe 一致；
201 for (packet = 0U; packet < PACKET_LIMIT; ++packet) {
207     Xil_DCacheFlushRange(...); Xil_DCacheInvalidateRange(...);
→ 注意：本文件用 cacheable 路径（未做 non-cacheable 映射），故每笔都 flush+invalidate。这是与 probe 系列（默认 non-cacheable）的可见差异。
211     XAxiDma_SimpleTransfer(... S2MM_MAX_BTT ...);
218     for (i=0; i<POLL_LIMIT; i++) if (!XAxiDma_Busy(...)) break;
→ 这里**仍用** XAxiDma_Busy() 判完成（与 probe 的“直接读 DMASR”不同）。但因为随后第 223–227 行会再读 DMASR 并显式检查 Halted/Error/Idle，所以即便 Busy 在 Halted 时返 TRUE，第 225 行也会在 i==POLL_LIMIT 时判失败，不会“假装成功”。不过它仍会把“出错”误等成一整轮 POLL_LIMIT 才报错——属轻微保守。
224     bytes = dma_reg_read(S2MM_LENGTH_OFFSET);
225     if ((i==POLL_LIMIT)||(dmasr&ERROR_MASK)||(dmasr&HALTED)||!(dmasr&IDLE)) park_after_failure("S2MM timeout, halt, or AXI error");
229     if ((bytes==0U)||(bytes>S2MM_MAX_BTT)||((bytes&7U)!=0U)) park_after_failure("S2MM returned an invalid byte count");
232     Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
233     if (inspect_packet(...) != XST_SUCCESS) park_after_failure("event packet contract failed");
236~240 累计 bytes min/max/total。
243 收满 128 包后打印 FEATURE_DMA_S2MM_PASS 并 park。
```

### 易错点小结（`pd_feature_dma_s2mm_smoke.c`）

1. **用 cacheable 路径 + 每笔 flush/invalidate**（未做 non-cacheable 映射），与 probe 默认不同；务必保留这两行，否则可能读到旧 cache 内容。
2. **仍用 `XAxiDma_Busy()` 作轮询退出条件**：虽随后有 DMASR 硬检查兜底，但出错时仍会自旋满 POLL_LIMIT 才报；属保守实现，不影响正确性。
3. 它是“特征→DMA”通路的**最小验收**，不验证快照/滤波，失败时应回退到此确认基础通路。
4. `inspect_packet` 的“末字必为 cycle”是硬契约，若 RTL 改变了 TLAST 来源需同步改此判定。

---

## 六、`pd_filter_axil_smoke.c`（滤波器 AXI-Lite 冒烟，63 行）

### 文件定位与演进关系

最轻量的 pd_filter_0 可达性检查：**不 arm DMA、不使能/清除滤波器**，只确认 `pd_filter_0` 的 AXI-Lite 寄存器窗口可读、且复位后默认处于“全局旁路 + 每通道旁路”的安全态。它是 `pd_filter_apply.c` 的前置 sanity check（确保 XSA 已含滤波器 IP、基址正确、默认安全）。

### 硬件地址清单（Q2）

| 宏 / 数值 | 地址（相对 `PD_FILTER_BASE`） | 含义 |
|---|---|---|
| `PD_FILTER_BASE` | `0x4002_0000`（或 `XPAR_PD_FILTER_0_*`） | `pd_filter_0` AXI-Lite 基址 |
| `FCTRL` | `0x000` | 滤波器全局控制 |
| `FSTATUS` | `0x004` | 滤波器状态 |
| `FBYPASS_MASK` | `0x008` | 每通道旁路掩码 |
| `FSAMPLE_HZ` | `0x00C` | 采样率（Hz） |
| `FILTER_VERSION` | `0x04`（作为比较常量） | 版本号（期望 0x04） |

### 数据流向（Q3）

无数据流。纯寄存器读取（`Xil_In32`）与判定，不写 DDR、不碰 DMA。

### 通过判据（Q4）

成功末行（第 61 行）：
```
PASS: AXI aperture responds; default IIR path is safely bypassed.
```
失败任一行：
```
FAIL: global bypass is not asserted; do not run DMA regression.   (ctrl&1==0)
FAIL: per-channel bypass is not 0xF; do not run DMA regression.   (mask&0xF != 0xF)
FAIL: unexpected filter version.                                  (status>>8 & 0xFF != 4)
FAIL: unexpected sample rate.                                     (sample_hz != 26000000)
```
即：默认必须“全局旁路且四通道全旁路”，版本=4，采样率=26 MHz。

### 逐行注释

```
9  #if defined(XPAR_PD_FILTER_0_S_AXI_BASEADDR)
10 # define PD_FILTER_BASE XPAR_PD_FILTER_0_S_AXI_BASEADDR
14 #elif defined(XPAR_PD_FILTER_0_BASEADDR)
15 # define PD_FILTER_BASE XPAR_PD_FILTER_0_BASEADDR
19 # define PD_FILTER_BASE 0x40020000U   /* 固定 BD 分配兜底 */
→ 三级回退：优先用 XSA 导出的宏，最后才用硬编码 0x40020000（与已知事实 pd_filter_0=0x4002_0000 一致）。注释提醒“即便走兜底，也必须重新导出 XSA”。
```

```
22~27 寄存器偏移与版本常量（见上表）。
```

```
29 int main(void)
36 ctrl = Xil_In32(PD_FILTER_BASE + FCTRL);
37 status = Xil_In32(PD_FILTER_BASE + FSTATUS);
38 mask = Xil_In32(PD_FILTER_BASE + FBYPASS_MASK);
39 sample_hz = Xil_In32(PD_FILTER_BASE + FSAMPLE_HZ);
→ 只读四个寄存器，确认窗口可达。
44 if ((ctrl & 1U) == 0U) { FAIL global bypass... }
48 if ((mask & 0xFU) != 0xFU) { FAIL per-channel bypass... }
52 if (((status >> 8) & 0xFFU) != FILTER_VERSION) { FAIL version... }
56 if (sample_hz != 26000000U) { FAIL sample rate... }
→ 四项校验全部通过才打印 PASS。特别强调：若默认不是“全旁路”，则“不要跑 DMA 回归”——因为未标定滤波器若直通会污染特征数据。
61 xil_printf("PASS: AXI aperture responds; default IIR path is safely bypassed.\r\n");
→ 注意：本文件**不写**任何寄存器（不清除、不使能），符合“非侵入”定位。
```

### 易错点小结（`pd_filter_axil_smoke.c`）

1. **仅读不写**：不要把它当成“配置滤波器”，它只是可达性 + 安全态检查。
2. **版本/采样率硬编码**：`FILTER_VERSION=4`、`26000000` 是构建契约；若 RTL 改版本号或采样率，此处会 FAIL（这是有意为之的契约守卫）。
3. 失败即意味着“不要在默认态下跑 DMA 回归”，应先解决 XSA / bitstream 一致性。

---

## 七、`pd_filter_apply.c`（IIR 系数安全下发，87 行）

### 文件定位与演进关系

这是**标定专属**程序：把一组 26 MSPS 的 IIR 系数安全写入 `pd_filter_0`，并验证“系数已 APPLY 生效”。它刻意**不**属于采集服务（采集服务不写系数）。设计铁律：**任何验证失败都把滤波器重新置为旁路**，绝不留下“半配置且直通”的危险态。

### 硬件地址清单（Q2）

| 宏 / 数值 | 地址（相对 `FILTER_BASE=0x4002_0000`） | 含义 |
|---|---|---|
| `FCTRL` | `0x000` | 全局控制（bit0=全局旁路？ bit1=APPLY 触发？） |
| `FSTATUS` | `0x004` | 状态（bit1=`coef_dirty`；bit15:8=版本） |
| `FBYPASS_MASK` | `0x008` | 每通道旁路掩码（0xF=四通道全旁路） |
| `FSAMPLE_HZ` | `0x00C` | 采样率 |
| `CH_STRIDE` | `0x200` | 每通道寄存器块间隔 |
| `COEF_BASE` | `0x014` | 系数数组基址（相对滤波器基址） |
| `COEF_STRIDE` | `0x004` | 单个系数间隔（每系数 4 字节） |
| `FILTER_VERSION` | `0x04`（常量） | 期望版本 4 |
| 每通道 shadow 使能 | `0x010 + ch*0x200` | 通道级影子寄存器使能（off=0 关闭） |
| 每通道系数 i | `0x014 + ch*0x200 + i*0x004` | 第 ch 通道第 i 个系数 |

### 数据流向（Q3）

无 DDR/DMA 流。数据流向是：C 数组 `coef[5]` → `Xil_Out32` 写 `pd_filter_0` 的各通道系数影子寄存器 → 触发 APPLY → 读 `FSTATUS` 回读校验。全部发生在 AXI-Lite 写/读。

### 通过判据（Q4）

成功末行（第 85 行）：
```
FILTER_APPLY_VERIFY_PASS
```
失败：任何 `check()` 不一致或状态校验失败，打印形如 `FAIL: readback off=... got=... exp=...` 或 `FAIL: coefficient dirty/version status=...` 或 `FAIL: APPLY did not clear coef_dirty, status=...`，并先调用 `safe_bypass()` 把滤波器重新旁路再返回。

> 关键契约（已知事实复核）：**生效判据是 `FSTATUS[1]`（`coef_dirty`）由 1 变 0**。`check()` 只验证“写进影子寄存器的值被回读一致”，那只证明写到了影子寄存器，**不代表已 APPLY**。真正生效要看 APPLY 后 `coef_dirty` 是否被硬件清零（第 71–75 行）。

### 逐行注释

```
10 #define FILTER_BASE 0x40020000U
→ 硬编码 0x40020000（与 pd_filter_0 一致，且要求 XSA 导出时滤波器基址确为此值）。
11 #define FCTRL 0x000U
12 #define FSTATUS 0x004U
13 #define FBYPASS_MASK 0x008U
14 #define FSAMPLE_HZ 0x00CU
15 #define CH_STRIDE 0x200U
16 #define COEF_BASE 0x014U
17 #define COEF_STRIDE 0x004U
18 #define FILTER_VERSION 0x04U
→ 寄存器布局。注意：通道块从 0x010 起、间隔 0x200；系数基址 0x014 = 0x010 + 0x004，即“通道块内偏移 0x004 起放系数”，与 CH_STRIDE 自洽。
```

```
21 static const unsigned coef[5] = { 0x01916U, 0x00000U, 0x3E6EAU, 0x23385U, 0x0CDD4U };
→ 5 个 Q1.16 系数（低 18 位有效），针对 26 MSPS、100 kHz~1 MHz。这是已标定好的常量，硬编于此；改系数要同步改这里。
```

```
23 static int check(unsigned off, unsigned expect)
→ 通用回读校验：读 FILTER_BASE+off，与 expect 不等则打印 FAIL 并返回 0。注意它读的是**影子/寄存器当前值**。
```

```
33 static int safe_bypass(void)
35     Xil_Out32(FILTER_BASE + FCTRL, 1U);          /* 全局旁路使能 */
36     Xil_Out32(FILTER_BASE + FBYPASS_MASK, 0xFU); /* 四通道全旁路 */
37     return check(FCTRL,1U) && check(FBYPASS_MASK,0xFU);
→ 安全态：全局旁路 + 每通道旁路。任何失败处理都先调它，保证“不标定成功就保持旁路”，不污染特征数据。
```

```
40 int main(void)
45     if (!safe_bypass()) return XST_FAILURE;
→ 第一步先旁路，确保起点安全。
46     if (Xil_In32(FILTER_BASE + FSAMPLE_HZ) != 26000000U) { FAIL unexpected sample rate; return; }
→ 确认采样率真的是 26 MHz（与系数匹配），否则拒绝继续。
```

```
51 for (ch = 0; ch < 4; ++ch) {
52     off = 0x010U + ch*CH_STRIDE;
53     Xil_Out32(FILTER_BASE + off, 0U);   /* shadow enable off：先关本通道影子使能 */
54     for (i = 0; i < 5; ++i) {
55         unsigned woff = COEF_BASE + ch*CH_STRIDE + i*COEF_STRIDE;
56         Xil_Out32(FILTER_BASE + woff, coef[i]);
57         if (!check(woff, coef[i])) { safe_bypass(); return XST_FAILURE; }
→ 逐通道、逐系数写入并立即回读校验。注意：先关 shadow 使能（off=0x010+ch*0x200 写 0），再写系数；回读一致只证明“写进影子寄存器”，**不等于已 APPLY**（见第 71 行真正判据）。任何不一致立即 safe_bypass 退出。
```

```
61 status = Xil_In32(FILTER_BASE + FSTATUS);
62 if (((status >> 8) & 0xffU) != FILTER_VERSION || !(status & 0x2U)) {
63     xil_printf("FAIL: coefficient dirty/version status=0x%08x\r\n"); safe_bypass(); return;
→ 写系数后，FSTATUS 应满足：版本字段(bit15:8)=4，且 coef_dirty(bit1)=1（表示“有脏系数待 APPLY”）。若版本不对或 dirty 未置位，说明硬件状态异常，safe_bypass 退出。
```

```
67 for (ch = 0; ch < 4; ++ch)
68     Xil_Out32(FILTER_BASE + 0x010U + ch*CH_STRIDE, 1U);
→ 打开每通道 shadow 使能（写 1）。使能后硬件才把影子系数真正提交/锁存。
```

```
70 Xil_Out32(FILTER_BASE + FCTRL, 0x3U);   /* 0x3 = bit0(保持旁路?) + bit1(APPLY) */
→ 【观察】4（推测）：FCTRL 写 0x3。结合上下文：在此之前 FCTRL=1（只旁路），此处写 0x3=bit0+bit1，注释自称“keep global bypass, APPLY”。即 bit0=全局旁路保持、bit1=APPLY 触发。APPLY 是一次性触发，让硬件把 shadow 系数生效并（若成功）清 coef_dirty。
71 status = Xil_In32(FILTER_BASE + FSTATUS);
72 if (status & 0x2U) {
73     xil_printf("FAIL: APPLY did not clear coef_dirty, status=...\r\n"); safe_bypass(); return;
→ **真正生效判据**：APPLY 后 coef_dirty(bit1) 必须为 0。若仍为 1，说明 APPLY 未成功（硬件未接受系数），立即 safe_bypass 退出。这一步正是“读回一致≠已 APPLY”的关键区别所在。
```

```
77 Xil_Out32(FILTER_BASE + FCTRL, 0U);          /* 清全局旁路 + 清 APPLY 位 */
78 Xil_Out32(FILTER_BASE + FBYPASS_MASK, 0U);   /* 取消每通道旁路 → 滤波器正式直通 */
79 status = Xil_In32(FILTER_BASE + FSTATUS);
80 if (((status >> 8) & 0xffU) != FILTER_VERSION) { FAIL post-apply version; safe_bypass(); return; }
→ 最后解除旁路使滤波器生效，并再次确认版本未变（防止 APPLY 过程中状态异常）。
85 xil_printf("FILTER_APPLY_VERIFY_PASS\r\n");
→ 全部通过，打印通过标志。此时滤波器已在 26 MSPS 下以给定系数正式工作，且版本正确。
```

### 易错点小结（`pd_filter_apply.c`）

1. **`check()` 回读一致 ≠ 已 APPLY**：只是证明写到影子寄存器；真正生效看 APPLY 后 `coef_dirty(bit1)` 清零（第 72 行）。这是本文件最重要的契约。
2. **任何失败都 safe_bypass**：绝不留“半配置直通”危险态，设计正确且必须保留。
3. **`FCTRL` 位定义是推测**：bit0=全局旁路、bit1=APPLY 来自代码行为推断（源码无位名宏）。若 RTL 实际位定义不同，需以 RTL 为准；但“APPLY 后 coef_dirty 清零”是更可靠的判据，不依赖此推断。
4. **系数与采样率硬编码**：`coef[5]`、`26000000`、`FILTER_VERSION=4` 都是标定契约；改任一项必须同步 RTL 与这里。
5. **通道 shadow 使能先关后开**：第 53 行先写 0 关闭、第 68 行写 1 打开，顺序不可颠倒，否则可能把未就绪的影子值提前提交。

---

## 八、`pd_capture_service.c`（首个集成级采集服务，301 行）

### 文件定位与演进关系

这是**首个把两条已验证路径合在一起的集成服务**：
1. `pd_feature_0/m_axis` → AXI DMA S2MM → PS DDR（特征流）
2. `event_accept` → 自动原始 DDR 快照 → 快照槽描述符 → PS 锁/读/释放

它**不写**滤波系数/特征阈值（避免误覆盖未标定参数），是**有界**（128 包）的集成验收，通过打印 `CAPTURE_SERVICE_PASS`。它比 `pd_acquisition_service.c` 更“集成测试”取向（中途只读快照首/末字、不落 PS 归档）；`pd_acquisition_service.c` 则更“完整常驻骨架”（带环形归档、UART 控制）。二者演进上：`pd_capture_service` 是较早的集成验证，`pd_acquisition_service` 是其“产品化”版本。

### 硬件地址清单（Q2）

除 DMA（`0x4040_0000`，S2MM `0x30/0x34/0x58`）外，新增 `pd_ddr_0`：

| 宏 / 数值 | 地址（相对 `PD_DDR_BASE=0x4001_0000`） | 对应 PL 寄存器 |
|---|---|---|
| `DDR_CTRL` | `0x000` | DDR/环采集控制 |
| `DDR_STATUS` | `0x004` | DDR/环状态 |
| `FREEZE_CTRL` | `0x034` | 冻结/恢复控制（写 2 似为 W1P 恢复） |
| `SLOT_CTRL` | `0x04C` | 快照槽命令（锁/释放/清） |
| `SLOT_STATUS` | `0x050` | 快照槽状态 |
| `SLOT_SEQ` | `0x054` | 硬件槽序列号 |
| `SLOT_DROPS` | `0x058` | 硬件拒绝（drop）计数 |
| `SNAP_TRIG_CTRL` | `0x05C` | 自动快照触发控制 |
| `SLOT_BASE_OFF(n)` | `0x060 + n*0x10` | 第 n 槽物理基址 |
| `SLOT_LEN_OFF(n)` | `0x064 + n*0x10` | 第 n 槽长度 |
| `SLOT_SEQ_OFF(n)` | `0x068 + n*0x10` | 第 n 槽序列 |
| `SLOT_FLAGS_OFF(n)` | `0x06C + n*0x10` | 第 n 槽标志 |

位定义：
- `DDR_STATUS_ERR (1<<5)`、`DDR_STATUS_COPY_ERR (1<<8)`、`DDR_STATUS_CFG_ERR (1<<9)`
- `SLOT_VALID_MASK 0x0000000F`（bit0-3，哪槽有效）、`SLOT_BUSY_MASK 0x000000F0`（bit4-7，哪槽忙）、`SLOT_FULL (1<<12)`、`SLOT_CFG_ERR (1<<13)`、`SLOT_REQ_OVERFLOW (1<<14)`、`SLOT_CMD_ERR (1<<15)`
- `SLOT_LAST_SHIFT 17`（选中槽号在 bit17-18，`&0x3`）、`SLOT_READY (1<<19)`
- `TRIG_ENABLED (1<<0)`、`TRIG_MASK_ALL (0xF<<1)`、`TRIG_ARMED (1<<16)`

RX 缓冲：同前 `0x0110_0000`。注意本文件**没有**定义 EVENT/SNAP 归档区（不落 PS 归档）。

### 数据流向（Q3）

- 特征流：`pd_feature_0/m_axis` → S2MM → `0x0110_0000` → 程序读前若干字统计 peak/cycle。
- 原始快照：`event_accept` 触发 PL 把原始 DDR 环某段冻结进四槽之一（`0x2000_1000~0x2300_1000`，每槽 12 MiB）→ `service_snapshot` 读 `SLOT_*` 描述符，invalidate 槽区，**仅读首/末字打印**（不复制），释放槽、恢复冻结。

### 通过判据（Q4）

成功末行（第 294–297 行）：
```
CAPTURE_SERVICE_PASS packets=128 snapshots=<非零> peak_words=... cycle_words=<≥128> bytes_avg=<非零> ...
```
途中必备：
```
SNAP 0 slot=<0..3> base=2000xxxx len=<8 字节对齐且非零> first=... last=...
CLEAN_STOP ddr_status=00000000 slot_status=<无 busy 位> polls=...
```
失败打印 `CAPTURE_SERVICE_FAIL: <reason>` 并 WFI 停机（第 114–123 行 `stop_fail`）。

### 逐行注释（要点）

```
99  #define RX_BUFFER_BASE       ((UINTPTR)PS_DDR_BASE + 0x01000000U)
100 #define S2MM_MAX_BTT         65528U
101 #define PACKET_LIMIT         128U
→ 与 smoke 一致：BTT 65528、有界 128 包。
103 #define SNAP_SLOT_SIZE       0x00C00000U   /* 12 MiB */
104 #define SNAP_SLOT_LOW        0x20001000U
105 #define SNAP_SLOT_HIGH       0x23001000U
→ 快照槽物理窗口：[0x2000_1000, 0x2300_1000)，与已知事实一致。用于校验槽描述符的 base/len 落在此窗口内。
```

```
127 static void configure_capture(void)
131 ddr_write(DDR_CTRL, 0U);
132 ddr_write(FREEZE_CTRL, 2U);
→ 先停 DDR/环采集，并写 FREEZE_CTRL=2（推测为 W1P“释放/恢复冻结状态”）。
133 for (i=0; i<1000000U; i++) if ((ddr_read(DDR_STATUS) & (1U|DDR_STATUS_ERR))==0) break;
→ 等待 DDR 状态“stale ring 错误位(1|bit5)”清零；超时则 stop_fail("stale ring error did not clear")。注意这里轮询上限 100 万（比 probe 小），且只查 bit0 与 bit5。
141 ddr_write(SLOT_CTRL, (1U << 12));
→ 写 SLOT_CTRL bit12 = SLOT_FULL？注释说是“Clear slot/trig sticky state”（清 sticky）。【观察】5（推测）：bit12 在宏里叫 SLOT_FULL 但此处用作“清 sticky”，位语义与命名不完全一致，属 PL 契约未在本文件展开说明之处。
142 ddr_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED);
143 ddr_write(DDR_CTRL, 1U);
144 ddr_write(SLOT_CTRL, 1U);
145 ddr_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);
→ 【观察】6（推测）：SNAP_TRIG_CTRL 先写 `(1<<8)|mask|enable`，再去掉 `(1<<8)` 重写成 `mask|enable`。bit8 不在任何具名宏里，推测是“软件触发/清 arm 脉冲”（先脉冲后置常arming）。与 pd_acquisition_service 的 configure_capture 同一手法。若 PL 契约变更需同步。
147 if ((ddr_read(SNAP_TRIG_CTRL) & (TRIG_ENABLED|TRIG_ARMED)) != (TRIG_ENABLED|TRIG_ARMED))
149     stop_fail("automatic snapshot trigger did not arm");
→ 校验触发已“使能且已布防(armed)”，否则失败。
```

```
155 static void service_snapshot(u32 *last_seq, u32 *snapshot_count)
→ 单槽生命周期：READY 跃迁 → 锁槽 → 读描述符 → invalidate → 读首末字 → 释放 → 恢复冻结。
166 if (!(status & SLOT_READY) || seq == *last_seq) return;
→ 没有 READY 或序列号未变（无新快照）→ 直接返回，不处理。
169 slot = (status >> SLOT_LAST_SHIFT) & 0x3U;
170 if (!(status & (1U << slot))) stop_fail("READY without a valid selected slot");
→ 取“最近完成槽号”，并校验该槽的 valid 位确实置位（READY 但 selected 槽无效 = 矛盾，失败）。
173 ddr_write(SLOT_CTRL, 1U << (4U + slot));   /* 锁 W1P：bit(4+slot) */
175 if (!(status & (1U << (8U + slot)))) stop_fail("slot lock did not stick");
→ 写 SLOT_CTRL 的“锁”位（bit4+slot），回读校验锁定位（bit8+slot）置位；锁不住=失败。
178~181 读 BASE/LEN/SEQ/FLAGS 四个槽描述符；
182 if ((base&7U)||(len&7U)||!len||len>SNAP_SLOT_SIZE||base<SNAP_SLOT_LOW||base+len>SNAP_SLOT_HIGH)
184     stop_fail("invalid slot descriptor");
→ 描述符合法性：基址/长度 8 字节对齐、长度非零且不超 12 MiB、落在四槽窗口内。任一不满足=失败。
186 Xil_DCacheInvalidateRange((UINTPTR)base, len);
→ 关键：快照是 PL 直接写 DDR，CPU 读前必须 invalidate，否则读到旧 cache。
187 words = (volatile u32 *)(UINTPTR)base;
188 first = words[0]; last = words[(len/4U)-1U];
→ 只读首/末 32 位字打印（不像 acquisition 服务那样整段复制归档）。这是“集成冒烟”而非“归档服务”的体现。
193 ddr_write(SLOT_CTRL, 1U << (8U + slot));   /* 释放 W1P：bit(8+slot) */
195 if (ddr_read(SLOT_STATUS) & (1U << (8U + slot))) stop_fail("slot release did not clear lock");
→ 写释放位（bit8+slot），回读确认锁定位已清。
196 ddr_write(FREEZE_CTRL, 2U);                 /* 恢复/重新布防 W1P */
→ 恢复冻结逻辑，使下一事件能再产快照。
198 *last_seq = seq; (*snapshot_count)++;
```

```
202 static void receive_one_packet(u32 packet_index, u32 *peak_words, u32 *cycle_words, u32 *bytes_total)
→ 与 smoke 类似的单包接收：flush+invalidate → ack → SimpleTransfer(65528) → 轮询（这里用 XAxiDma_Busy）→ 读 DMASR/LENGTH → 校验 → invalidate(bytes) → 遍历统计 peak/cycle → 校验末字为 cycle。
220 if (i==POLL_LIMIT || (dmasr&(HALTED|ERROR_MASK)) || !(dmasr&IDLE) || !bytes||bytes>S2MM_MAX_BTT||(bytes&7U))
221     stop_fail("S2MM completion contract failed");
→ 比 smoke 多检查了“bytes 非 0 / 不超 BTT / 8 对齐”（与 inspect_packet 同款契约）。
231 if (((u32)(rx[words-1U]>>56)) != 1U) stop_fail("AXIS packet did not terminate on cycle statistics");
→ 末字必为 cycle（TLAST 契约）。
```

```
243 static void shutdown_capture_cleanly(void)
247 ddr_write(SNAP_TRIG_CTRL, 0U);  /* 停止接受新触发 */
248 ddr_write(DDR_CTRL, 0U);        /* 停原始环采集 */
249 ddr_write(FREEZE_CTRL, 2U);     /* 释放/恢复冻结 */
251 for (i=0; i<SHUTDOWN_POLL_LIMIT; i++) {
254     if (((ddr_status & (1U<<1))==0U) && ((slot_status & SLOT_BUSY_MASK)==0U)) {
256         xil_printf("CLEAN_STOP ddr_status=... slot_status=... polls=...\r\n"); return;
→ 干净停机判据：DDR 非 COPY_BUSY(bit1) 且无任何槽 busy(bit4-7)。注意**不**要求 SLOT_READY=0（READY 只是通知位，不代表在途传输）。超时则 stop_fail("raw DDR capture did not quiesce")。
```

```
264 int main(void)
286 for (packet = 0U; packet < PACKET_LIMIT; ++packet) {
287     receive_one_packet(packet, &peaks, &cycles, &bytes);
288     service_snapshot(&last_seq, &snapshots);
→ 每收一包特征，就尝试处理一个新快照。
290 service_snapshot(&last_seq, &snapshots);   /* 收尾再处理一次 */
292 shutdown_capture_cleanly();
294 xil_printf("CAPTURE_SERVICE_PASS packets=128 snapshots=... ...");
→ 打印 PASS 后 park（WFI）。
```

### 易错点小结（`pd_capture_service.c`）

1. **SNAP_TRIG_CTRL 的 bit8、SLOT_CTRL 的 bit12 位语义未在源码具名**：仅以字面量/SLOT_FULL 复用出现，属 PL 契约未展开处；改 PL 时需与 RTL 对齐（【观察】5、6）。
2. **快照只取首/末字**：本服务不复制整段快照到 PS 归档，因此不能当作“数据已落盘”的证据；真正归档在 `pd_acquisition_service.c`。
3. **`service_snapshot` 在每次收包后都调用**：即使无新快照也只是 return，成本低；但注意 `seq == *last_seq` 用“硬件槽序列号”去重，依赖 PL 每完成一次快照就自增 SLOT_SEQ。
4. **`receive_one_packet` 仍用 `XAxiDma_Busy()` 轮询**：同 smoke，出错时会自旋满 POLL_LIMIT 才被后续 DMASR 检查兜底。
5. **CLEAN_STOP 不要求 SLOT_READY=0**：与 `pd_acquisition_service` 的 quiesce 判据一致（仅查 VALID/BUSY/LOCKED 归零）。

---

## 九、`pd_acquisition_service.c`（整体式采集服务 V1，524 行）

### 文件定位与演进关系

这是**当前 DDS 原型的正式 PS 采集骨架（V1，整体式）**，也是 `pd_capture_service.c` 的“产品化”版本。它在 `pd_capture_service` 的两条路径之上增加：
- **环形归档**：事件归档（16×64 KiB）、快照归档（4×12 MiB），元数据存于全局 `g_acq`；
- **UART 命令协议**：START/STOP/STATUS/EVENT/SNAP/CLEAR/QUIT，支持受限运行（START 128）与连续运行（START 0）；
- **DMA 等待期间轮询 UART/网络**：使 STOP 在 DMA 等包时也能响应。

它**不写**滤波系数/特征阈值（与 capture 服务一致）。本文件经 `#ifndef PD_ACQ_NO_MAIN` 包裹 main，使其能被 `pd_acquisition_tcp_service_main.c` 以 `.inc` 形式包含（已 `diff` 确认 `.inc` 与 `.c` 逐字节相同）。当前活跃服务是 `sw/ps_service/`，故本文件属**早期整体式版本**。

### 硬件地址清单（Q2）

DMA 与 `pd_ddr_0` 部分与 `pd_capture_service.c` 基本相同，但位定义更完整，且新增**数据归档物理分区**与若干状态位：

- DMA：`axi_dma_0=0x4040_0000`，S2MM `0x30/0x34/0x58`；新增 `DMASR_ERROR_MASK 0x00004070`（bit4+5+6+14，含 ERR_IRQ）。
- `pd_ddr_0=0x4001_0000` 寄存器：DDR_CTRL/DDR_STATUS/FREEZE_CTRL/SLOT_CTRL/SLOT_STATUS/SLOT_SEQ/SLOT_DROPS/SNAP_TRIG_CTRL 及 SLOT_*_OFF(n) 同 capture。
- 新增 `pd_ddr_0` 状态位：`DDR_COPY_BUSY (1<<1)`、`DDR_RING_ERR (1<<5)`、`DDR_COPY_ERR (1<<8)`、`DDR_CFG_ERR (1<<9)`、`SLOT_LOCKED_MASK 0x00000F00`（bit8-11，锁定位）、`TRIG_ARMED (1<<16)`。
- **数据归档物理分区（绝对地址，硬编码）**：
  - `RX_BUFFER_BASE = PS_DDR_BASE + 0x01000000` → `0x0110_0000`，`RX_BUFFER_BYTES=65528`
  - `EVENT_ARCHIVE_BASE = 0x27000000`，`EVENT_ARCHIVE_COUNT=16`，`EVENT_ARCHIVE_STRIDE=0x00010000`（64 KiB）→ 区间 `0x2700_0000~0x2710_0000`
  - `SNAP_ARCHIVE_BASE = 0x24000000`，`SNAP_ARCHIVE_COUNT=4`，`SNAP_ARCHIVE_STRIDE=0x00C00000`（12 MiB）→ 区间 `0x2400_0000~0x2700_0000`
  - `SNAP_SLOT_LOW=0x20001000`，`SNAP_SLOT_HIGH=0x23001000`（PL 四槽物理窗口）

### 数据流向（Q3）

- **特征流**：`pd_feature_0/m_axis` → S2MM → `0x0110_0000` → `archive_event_packet` 校验后以 `memcpy` 复制到**事件归档** `0x2700_0000 + index*64KiB`，并 `Xil_DCacheFlushRange` 保证下位机/ DMA 读取一致。
- **原始快照**：PL 冻结进四槽之一（`0x2000_1000~0x2300_1000`）→ `archive_new_snapshot` 锁槽、读描述符、`Xil_DCacheInvalidateRange(base,len)` → `memcpy` 复制到**快照归档** `0x2400_0000 + index*12MiB` → `Xil_DCacheFlushRange` → 释放槽、恢复冻结。
- 元数据：每次归档写 `g_acq.event[]` / `g_acq.snapshot[]`（环形，下标 `seq % COUNT`），记录地址/长度/计数/`hw_slot_sequence`/flags，供上位机协议直接读取。

### 通过判据（Q4）

- 受限运行结束打印（第 514–516 行）：
  ```
  ACQ_SERVICE_PASS packets=128 events=128 snapshots=<≥0> ev_ovw=... snap_ovw=...
  ```
- 干净停机打印（第 447–448 行）：
  ```
  ACQ_CLEAN_STOP ddr=... slot=00080000 polls=... drained=...
  ```
  要求 `slot` 低 12 位（VALID+BUSY+LOCKED）为 0；bit19(SLOT_READY) 可保持 1（通知位非占用）；`drained>0` 表示停止前已接管的末尾快照被归档而非丢弃。
- 失败：任意硬件错误调用 `halt_failure`（第 278–286 行），打印 `ACQ_SERVICE_FAIL: <why> ddr=... slot=... dma=...` 并 `for(;;) wfi` 永久停机（**不复位**，保留现场）。

### 逐行注释（要点：重点在 capture 未涉及的部分）

```
122 typedef struct { u32 sequence,ddr_addr,bytes,peak_words,cycle_words; } pd_event_record_t;
130 typedef struct { u32 sequence,source_slot,source_addr,archive_addr,bytes,hw_slot_sequence,flags; } pd_snapshot_record_t;
140 typedef struct { u32 magic,version,event_sequence,snapshot_sequence,event_overwrites,snapshot_overwrites,dma_errors,slot_errors,last_dma_status,last_ddr_status,last_slot_status; pd_event_record_t event[16]; pd_snapshot_record_t snapshot[4]; } pd_acq_shared_t;
→ 全局元数据布局。magic=0x50444151("PDAQ")、version=1。环形覆盖计数 event_overwrites/snapshot_overwrites 记录“归档满后覆盖最旧”的次数——不是静默丢失，而是给上位机的证据。
```

```
158 volatile pd_acq_shared_t g_acq;
→ 全局且 volatile：它可能将来被上位机协议/调试器直接读取其地址；volatile 防止编译器把字段优化进寄存器而看不到更新。
159 static XAxiDma g_dma;
160 static volatile u32 g_running;        /* 运行态标志，被 UART/TCP 命令改写 */
161 static volatile u32 g_stop_request;   /* STOP 请求（volatile：跨轮询循环可见） */
162 static volatile u32 g_quit_request;
163 static volatile u32 g_start_request;
164 static volatile u32 g_start_limit;    /* 0=连续，非0=受限包数 */
→ 这些 volatile 全局是状态机的“命令信箱”。volatile 保证主循环与 UART/TCP 回调（中断上下文或不同轮询点）对它们的写入对主循环可见，不被优化掉。
```

```
182 static u32 parse_u32(const char *p, u32 default_value)
→ 极简无符号十进制解析（跳过前导空格，逐字符累乘）；无数字则返回 default。用于解析 START 后的包数、EVENT/SNAP 后的索引。无负号、无进制、无错误提示（非数字即默认），属“够用即可”的裸机解析。
```

```
219 static void clear_metadata(void)
221 if (g_running) { ERR CLEAR only while idle; return; }   /* 仅空闲可清 */
225 memset(&g_acq,0,sizeof(g_acq));
226 g_acq.magic = 0x50444151U; g_acq.version = 1U;
→ CLEAR 只清软件元数据、不碰 PL 数据区；重设 magic/version。
```

```
231 static void execute_command(char *line)
→ UART 命令分派：HELP/START [n]/STOP/STATUS/EVENT n/SNAP n/CLEAR/QUIT。START 设置 g_start_limit（默认 PD_ACQ_PACKET_LIMIT=128）与 g_start_request；STOP 置 g_stop_request；QUIT 置 g_quit_request（运行中先停后退）。
```

```
259 static void uart_poll(void)
261 while (XUartPs_IsReceiveData(UART_BASE)) {
262     char c = XUartPs_RecvByte(UART_BASE);
263     if (c=='\r'||c=='\n') { if (g_uart_len) { line[len]=0; execute_command(line); len=0; } }
269     else if (c==8||c==127) { if (len) --len; }   /* 退格/删除 */
271     else if (c>=' '&&c<='~') { if (len+1<UART_LINE_BYTES) line[len++]=c; else ERR too long; }
→ 轮询式命令行：逐字节收，回车提交，退格删，可打印字符入行（超 48 字节报错）。注意这是**轮询**而非中断，注释说明“在 DMA 等待里也轮询 UART，使 STOP 始终能响应”，且避免引入第二中断源。
```

```
278 static void halt_failure(const char *why)
285 for (;;) __asm__ volatile ("wfi");
→ 致命错误后永久停机。wfi 让 CPU 停发总线事务，便于调试器接入；不复位以便现场（DDR/槽/DMA 状态）保留。
```

```
288 static void configure_capture(void)
→ 与 pd_capture_service 结构相同：清 DDR/冻结 → 等 stale 状态清 → 清 SLOT sticky(bit12) → 脉冲 SNAP_TRIG_CTRL(bit8) → 使能 DDR/SLOT → 去 bit8 重 arm 触发 → 校验 TRIG_ENABLED|TRIG_ARMED。
298 if (i==SHUTDOWN_POLL_LIMIT) halt_failure("stale ring status did not clear");
→ 注意本文件复用 SHUTDOWN_POLL_LIMIT(100万) 作“启动等待上限”，与 capture 相同。
```

```
309 static void archive_event_packet(u32 bytes)
318 if (!bytes || bytes>RX_BUFFER_BYTES || (bytes&7U)) halt_failure("bad DMA byte count");
→ 字节数非零、≤65528、8 对齐。
319 for (i=0;i<words;i++) { type=rx[i]>>56; if(type==0)peaks++; else if(type==1)cycles++; else halt_failure("unknown event word type"); }
325 last_type = rx[words-1]>>56; if (last_type!=1) halt_failure("AXIS packet did not end in cycle word");
→ 遍历统计并校验末字为 cycle（TLAST 契约）。
328 memcpy((void*)dst, (const void*)RX_BUFFER_BASE, bytes);
329 Xil_DCacheFlushRange(dst, bytes);
→ 复制到事件归档并 flush（使后续从上位机/DMA 视角看到一致数据）。注意：源 RX 缓冲已在前一步 invalidate，这里从 DDR 读源，写归档后 flush 归档。
330 if (seq>=EVENT_ARCHIVE_COUNT) ++g_acq.event_overwrites;
312/313 u32 index = seq % EVENT_ARCHIVE_COUNT;
→ **环形归档**：下标 = seq % 16，覆盖最旧；满 16 后递增 event_overwrites（显式记录覆盖，不静默丢失）。
332~336 写 g_acq.event[index] 各字段，++g_acq.event_sequence。
```

```
341 static int receive_one_packet(void)
→ 返回 0 仅当“DMA 等待期间观测到 STOP”（此时复位 S2MM 丢弃半包）；返回 1 表示正常收到一包并已归档。
344 Xil_DCacheFlushRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
345 Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
→ 每笔前 flush+invalidate（本文件未做 non-cacheable 映射，故需 cache 维护）。
346 XAxiDma_IntrAckIrq(...);
347 XAxiDma_SimpleTransfer(... RX_BUFFER_BYTES ...);
350 for (i=0;i<DMA_POLL_LIMIT;i++) { uart_poll();
352 #ifdef PD_ACQ_NETWORK_POLL
355     PD_ACQ_NETWORK_POLL();
→ 关键：DMA 等待循环里**每轮都调 uart_poll() 与（若定义）网络轮询钩子**。这就是“STOP 在 DMA 等包时也能响应”的实现；也为 TCP 版（pd_acquisition_tcp_service_main.c）提供注入点。
357     if (g_stop_request) { XAxiDma_Reset(&g_dma); while(!ResetIsDone); return 0; }
→ 观测到 STOP：复位 S2MM（丢弃当前未完成的半包）并返回 0，由主循环执行 clean_stop。
362     if (!XAxiDma_Busy(...)) break;
367 if (i==DMA_POLL_LIMIT || (dmasr&(HALTED|ERROR_MASK)) || !(dmasr&IDLE)) { dma_errors++; halt_failure("DMA completion failed"); }
→ 同 smoke/capture：Busy 退出（Halted 时仍会返 TRUE，但随后 DMASR 检查会兜底判失败）。
372 Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
373 archive_event_packet(bytes); return 1;
```

```
377 static void archive_new_snapshot(u32 *last_slot_sequence)
→ 与 pd_capture_service.service_snapshot 几乎同构，但**多了整段 memcpy 到快照归档**并 flush：
408 Xil_DCacheInvalidateRange((UINTPTR)base, bytes);
409 memcpy((void*)dst, (const void*)(UINTPTR)base, bytes);
410 Xil_DCacheFlushRange(dst, bytes);
→ 把 PL 四槽内容整段复制到 PS 快照归档（0x2400_0000 区）。这是与 capture 服务的本质区别：本服务真正“落盘”归档。
406 index = g_acq.snapshot_sequence % SNAP_ARCHIVE_COUNT;  /* 环形，4 槽 */
411 if (g_acq.snapshot_sequence>=SNAP_ARCHIVE_COUNT) ++g_acq.snapshot_overwrites;
→ 满 4 后覆盖最旧并计数。
421 ddr_write(SLOT_CTRL, 1U<<(8U+slot));  /* 释放 */
423 ddr_write(FREEZE_CTRL, 2U);            /* 恢复冻结 */
```

```
432 static void clean_stop(u32 *last_slot_sequence)
436 ddr_write(SNAP_TRIG_CTRL, 0U); ddr_write(DDR_CTRL, 0U); ddr_write(FREEZE_CTRL, 2U);
439 for (i=0;i<SHUTDOWN_POLL_LIMIT;i++) {
442     archive_new_snapshot(last_slot_sequence);   /* 先排空“已接受但未归档”的槽 */
445     if (!(ddr&DDR_COPY_BUSY) && !(slot&SLOT_BUSY_MASK) && !(slot&SLOT_VALID_MASK)) {
447         xil_printf("ACQ_CLEAN_STOP ddr=... slot=... polls=... drained=...\r\n"); return;
→ quiesce 判据：DDR 非 COPY_BUSY 且无 BUSY 且无 VALID。注释特意说明“干净 DMA/槽 busy 不够，已完成的未归档槽是有效数据必须复制/释放”，故循环里反复 archive_new_snapshot 直到无 VALID 槽。
452 halt_failure("capture did not quiesce");
```

```
455 #ifndef PD_ACQ_NO_MAIN
456 int main(void)
→ 主循环（当被 TCP 版包含且定义 PD_ACQ_NO_MAIN 时，此 main 被整体跳过，由 TCP 版提供 main）。
461 memset(&g_acq,0,...); g_acq.magic=0x50444151; version=1;
469 cfg=XAxiDma_LookupConfig(DMA_LOOKUP_ARG); ... CfgInit ... HasSg 守卫 ... IntrDisable ... Reset ...
→ 标准 DMA 初始化。
477 xil_printf("READY: type HELP, then START 128 ...");
478 while (!g_quit_request) {
479     uart_poll();
481     if (!g_running) {
482         if (!g_start_request) continue;
483         g_start_request=0; g_stop_request=0; packets=0;
486         configure_capture();
487         last_slot_sequence = ddr_read(SLOT_SEQ);   /* 以当前硬件序列为基准，避免把旧快照当新 */
488         g_running=1U; ... continue;
→ 空闲态：收到 START 请求 → 配置采集 → 记录当前 SLOT_SEQ 作为去重基准 → 进入运行。
493     if (g_stop_request || !receive_one_packet()) {
494         archive_new_snapshot(...); clean_stop(...); g_running=0; ... continue;
→ 运行态：STOP 或 收到返回 0（DMA 中等到 STOP）→ 收尾归档 + 干净停机。
503     archive_new_snapshot(...); ++packets;
505     if ((packets%REPORT_PERIOD)==0U) 打印 ACQ 进度（REPORT_PERIOD=32）；
510     if (g_start_limit!=0U && packets>=g_start_limit) {
512         archive_new_snapshot(...); clean_stop(...); g_running=0;
514         xil_printf("ACQ_SERVICE_PASS packets=... events=... snapshots=... ...");
→ 受限运行达到上限 → 收尾 + 打印 PASS。连续运行（g_start_limit==0）则永不触发此分支，直到 STOP。
```

### 易错点小结（`pd_acquisition_service.c`）

1. **两个归档区首尾紧邻、无保护间隙**：`SNAP_ARCHIVE_BASE=0x2400_0000`、stride 12 MiB、4 槽 → 止于 `0x2700_0000`；`EVENT_ARCHIVE_BASE` 正好从 `0x2700_0000` 起。安全性完全依赖 `bytes <= SNAP_ARCHIVE_STRIDE` 的校验（第 402 行）；若将来 PL 快照实际长度偶发超过 12 MiB，会越界踩到事件归档（【观察】事实：区间相邻无 guard）。
2. **SNAP_TRIG_CTRL bit8 / SLOT_CTRL bit12 位语义未具名**：同 capture 服务的【观察】5、6；改 PL 需对齐。
3. **`receive_one_packet` 仍用 `XAxiDma_Busy()` 轮询**：虽随后有 DMASR 兜底，但出错自旋至 2000 万次才报；属保守但可接受。
4. **`g_start_limit==0` 表示连续运行**：`START 0` 永不自动停，必须 STOP；这与 UART/TCP 帮助文本一致，但代码里“0=连续”是隐含约定，新人易误以为是“0 包”。
5. **环形覆盖是显式计数而非静默丢失**：event_overwrites/snapshot_overwrites 在状态/PASS 行都打印，上位机接入前靠它发现“数据未被及时取走”。
6. **volatile 全局是命令信箱**：g_running/g_stop_request 等必须 volatile，否则编译器可能把主循环里的读取优化掉，导致 STOP 不响应。
7. **本文件被 TCP 版以 `.inc` 形式包含**：必须保留 `#ifndef PD_ACQ_NO_MAIN` 包裹 main，否则 TCP 版会出现“两个 main / 两个 g_acq”的重复定义链接错误（README 已强调 `.inc` 不可改回 `.c`）。

---

## 十、`pd_acquisition_tcp_service_main.c`（采集引擎 + lwIP TCP V2，484 行）

### 文件定位与演进关系

这是 `pd_acquisition_service.c`（采集引擎）接入 **lwIP TCP** 的版本：
- 第 37–42 行：`#define PD_ACQ_NO_MAIN` + `#define PD_ACQ_NETWORK_POLL pd_tcp_network_poll` + `#include "pd_acquisition_service.inc"`。即把 V1 引擎（与 `.c` 逐字节相同）整体包含进来，但跳过其 main、并把“网络轮询钩子”接到本文件的 `pd_tcp_network_poll`。
- 本文件提供自己的 `main()`：初始化 lwIP（静态 IP `192.168.1.10`、端口 `6001`）、启动 TCP 监听，其余采集状态机完全复用引擎。
- 保留 UART 命令行作为“断网恢复/调试口”。

它比 `pd_acquisition_tcp_service_使用说明.md` 描述的“V1（仅控制/元数据）”更进一步：本源码（与 `TCP数据下载V2_协议与使用.md` 对应）已加入 `GET EVENT/SNAP` 的**原始数据分块下载**（V2）。当前活跃服务是 `sw/ps_service/`，故本文件是“旧版 TCP 接入”形态。

> 已 `diff` 确认：`pd_acquisition_tcp_service_main.c` 与 `tcp_service_template/main.c` 逐字节相同；`pd_acquisition_service.inc` 与 `pd_acquisition_service.c` 逐字节相同。所以本文件的“引擎部分”就是第九节的 V1 源码，下面只注解 **lwIP/TCP 专属部分**，引擎部分请回看第九节。

### 硬件地址清单（Q2）

lwIP 部分不新增 PL 地址；采集引擎部分同第九节（DMA `0x4040_0000`、pd_ddr `0x4001_0000`、归档分区同前）。TCP 相关常量：

| 宏 | 值 | 含义 |
|---|---|---|
| `PD_TCP_PORT` | `6001U` | TCP 监听端口 |
| `PD_TCP_LINE_BYTES` | `96U` | TCP 命令行缓冲（95 字节 + 1 终止符） |
| `PD_TCP_GET_MAX_BYTES` | `16384U` | 单次 GET 上限 16 KiB |
| `PD_TCP_SEND_BYTES` | `1024U` | 每次 tcp_write 发送 1 KiB |
| `PD_TCP_MAC0..5` | `00:0A:35:00:01:02` | 静态 MAC |

### 数据流向（Q3）

- 控制/元数据：`TCP 命令 → lwIP recv 回调 → tcp_execute_command → 读 g_acq → tcp_reply(ASCII)`。
- 原始下载：`GET EVENT/SNAP index offset bytes →` 计算归档区物理地址 `base` → `tcp_transfer_pump` 分块从 PS DDR 归档区 `tcp_write`（每次 1 KiB，带 `TCP_WRITE_FLAG_COPY`）→ 客户端先收一行 `DATA V2 ... crc32=...` 头，再收 `bytes` 个裸字节。
- 注意：GET 只读 **PS 归档区**（`0x2400_0000` 快照 / `0x2700_0000` 事件），不读 PL 正在写的四槽（`0x2000_1000~0x2300_1000`），避免读到在途数据。

### 通过判据（Q4）

- 连接建立后首行：`PD_ACQ TCP V2 READY; type HELP`（第 365 行 `pd_tcp_accept_cb`）。
- 无显式 PASS 字符串（常驻服务）。控制命令回显 `OK ...` / `ERR ...`。
- 数据下载的验收在下位机脚本侧：`DOWNLOAD_PASS bytes=3120000 file=...`（见 `TCP数据下载V2_协议与使用.md`），要求文件大小与 `SNAP 0` 返回的 `bytes` 完全一致。
- 引擎侧失败仍是 `ACQ_SERVICE_FAIL` / `halt_failure`（WFI 停机）；lwIP 初始化失败（如 `xemac_add` 失败）打印 `TCP_ACQ_FAIL: xemac_add failed` 并 WFI。

### 逐行注释（仅 TCP 专属部分）

```
36 static void pd_tcp_network_poll(void);
37 #define PD_ACQ_NO_MAIN
38 #define PD_ACQ_NETWORK_POLL pd_tcp_network_poll
42 #include "pd_acquisition_service.inc"
→ 三连：声明钩子、定义 PD_ACQ_NO_MAIN（让 .inc 里的 main 被跳过）、把网络轮询接到本函数、包含引擎。这是“复用引擎 + 自供 main”的关键写法。
```

```
44 #define PD_TCP_PORT          6001U
45 #define PD_TCP_LINE_BYTES    96U
46 #define PD_TCP_GET_MAX_BYTES 16384U
47 #define PD_TCP_SEND_BYTES    1024U
48~53 PD_TCP_MAC0..5  (00 0A 35 00 01 02)
→ TCP 参数。MAC 为局部管理地址（02 结尾），静态指定避免依赖 DHCP。
```

```
58 struct netif echo_netif;
→ lwIP 网络接口实例。注释说明：Vitis lwIP echo 模板的 platform.c 在其定时器回调里引用这个**全局名**，保留它才能复用生成的平台支持。
59 static struct tcp_pcb *g_client;
→ 当前已连接客户端（仅支持单连接；accept 时若已有连接则拒绝新连接，见第 356 行）。
60 static char g_tcp_line[PD_TCP_LINE_BYTES];
61 static u32 g_tcp_len;
→ TCP 侧命令行缓冲（类比 UART 的 g_uart_line）。
```

```
63 typedef struct { UINTPTR addr; u32 remaining; u32 offset; u32 bytes; u32 active; } pd_tcp_transfer_t;
71 static pd_tcp_transfer_t g_transfer;
→ GET 大块传输的状态机：addr=归档基址、offset=已发偏移、remaining=剩余、active=进行中。把“长传输”拆成多次 1 KiB 发送，避免单次 tcp_write 撑爆 lwIP 缓冲。
```

```
73 extern volatile int TcpFastTmrFlag; extern volatile int TcpSlowTmrFlag;
77 void tcp_fasttmr(void); void tcp_slowtmr(void);
→ lwIP RAW 模式模板导出的定时器入口（公开头未声明，故在此 extern/前向声明）。TcpFastTmrFlag/TcpSlowTmrFlag 由平台定时器中断置位，主循环里消费并调用对应 tmr。
```

```
80 static int tcp_reply(const char *text)
88 if (tcp_sndbuf(g_client) < len) { 打印 "TCP reply dropped: send buffer full"; return -1; }
92 err = tcp_write(g_client, text, len, TCP_WRITE_FLAG_COPY);
94 (void)tcp_output(g_client);
→ 发 ASCII 响应：先用 tcp_sndbuf 探缓冲余量（不足则丢弃并报错，不阻塞），再 tcp_write（COPY 标志让 lwIP 拷贝数据，调用方缓冲可复用），最后 tcp_output 立即推送。若不加 tcp_output，数据可能滞留直到定时器，响应变慢。
```

```
98 static u32 tcp_crc32(const u8 *data, u32 bytes)
100 u32 crc = 0xFFFFFFFFU;
103 crc ^= data[i];
104 crc = (crc&1U) ? ((crc>>1)^0xEDB88320U) : (crc>>1);
107 return ~crc;
→ 标准 IEEE CRC-32（多项式 0xEDB88320，查表法展开为逐位）。用于给下载数据头附校验和；注释说明首版 PC 脚本只记录元数据，下一阶段可强制校验。
```

```
110 static int tcp_read_u32(char **cursor, u32 *value)
116 parsed = strtoul(*cursor, &end, 0);
→ 用 strtoul 解析十进制/十六进制无符号数（base=0 自适应 0x 前缀）。带越界保护（>0xFFFFFFFF 视为错）。
```

```
123 static void tcp_transfer_pump(void)
128 if (!g_transfer.active || g_client==NULL) return;
129 chunk = (remaining>PD_TCP_SEND_BYTES)?PD_TCP_SEND_BYTES:remaining;  /* 每次最多 1 KiB */
131 if (tcp_sndbuf(g_client) < chunk) return;   /* 缓冲不够就下次再发，不阻塞 */
133 Xil_DCacheInvalidateRange(g_transfer.addr + g_transfer.offset, chunk);
→ 关键：从 PS DDR 归档区取数前必须 invalidate，否则 CPU 可能发旧 cache 行（数据一致性风险）。
134 err = tcp_write(g_client, addr+offset, chunk, TCP_WRITE_FLAG_COPY);
136 if (err==ERR_MEM) return;   /* 内存不足：保持 active，等下次泵送 */
137 if (err!=ERR_OK) { 打印 "TCP GET aborted"; g_transfer.active=0; return; }
142 g_transfer.offset += chunk; g_transfer.remaining -= chunk;
144 if (remaining==0) g_transfer.active=0;
145 (void)tcp_output(g_client);
→ 分块泵送：每次 1 KiB，ERR_MEM 时优雅退让（保持 active 等缓冲释放），其它错误则中止传输。这是“绝不在 TCP 回调里长时间阻塞”的实现（见 TCP 协议文档安全边界）。
```

```
148 static void tcp_reply_status(void) ...
163 static void tcp_reply_event(u32 index) ...
181 static void tcp_reply_snapshot(u32 index) ...
→ 与 UART 版 print_status/print_event_record/print_snapshot_record 对应，但改为 tcp_reply 输出 ASCII。索引越界或“无有效记录”返回 ERR。
```

```
200 static void tcp_start_get(char *line)
208 if (g_running) { tcp_reply("ERR GET is accepted only while idle"); return; }
→ 安全边界：只允许在采集停止（run=0）时下载，防止读到在途/被覆盖的归档。
213~236 解析 "GET EVENT|SNAP index offset bytes"，校验 index 有效、offset/bytes 合法（1..16384、不超记录边界）。
243 if (*p!=0 || offset>=available || bytes==0 || bytes>PD_TCP_GET_MAX_BYTES || bytes>available-offset)
245     tcp_reply("ERR GET range; bytes must be 1..16384 inside record"); return;
→ 范围守卫：bytes 在 1..16384 且完全落在记录内。
249 Xil_DCacheInvalidateRange(base+offset, bytes);
250 crc = tcp_crc32(base+offset, bytes);
251 snprintf(out, "DATA V2 kind=... index=... offset=... bytes=... crc32=...\r\n");
255 if (tcp_reply(out)!=0) return;
257~261 设置 g_transfer（addr/base, offset, remaining=bytes, active=1）。
→ 先发一行 DATA V2 头（含 CRC32），再启动分块泵送。客户端须先读完裸 payload 再发下一条命令（否则控制回复会与裸字节交错，第 266–270 行在 transfer.active 时直接忽略新命令正是为此）。
```

```
264 static void tcp_execute_command(char *line)
266 if (g_transfer.active) return;   /* 传输进行中忽略新命令，避免与裸字节交错 */
271~304 命令分派：HELP / START [n] / STOP / STATUS / EVENT n / SNAP n / GET ... / CLEAR / 未知。
277 g_start_limit = parse_u32(line+5, PD_ACQ_PACKET_LIMIT); g_start_request=1U;
→ 复用引擎的 parse_u32 与 g_start_request 信箱；START 经 TCP 与经 UART 等价。
```

```
307 static err_t pd_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
314 if (p==NULL) { tcp_recv(pcb,NULL); if(g_client==pcb) g_client=NULL; g_transfer.active=0; tcp_close(pcb); return ERR_OK; }
→ 对端关闭（p==NULL 表示连接断）：清客户端、中止在途传输、关 pcb。
322 tcp_recved(pcb, p->tot_len);   /* 告知 lwIP 已消费 pbuf，释放接收窗口 */
323 for (i=0;i<p->tot_len;i++) { pbuf_copy_partial(p,&c,1,i); ... 行缓冲逻辑同 uart_poll ... }
340 pbuf_free(p); return ERR_OK;
→ recv 回调：逐字节累积命令行（回车提交执行），用 pbuf_copy_partial 取单字节（简单但低效，足够命令协议）。注意：**实际数据下载不走这里**（走 g_transfer 泵送），回调只处理控制命令。
```

```
344 static void pd_tcp_err_cb(void *arg, err_t err)
348 g_client=NULL; g_tcp_len=0; g_transfer.active=0;
→ 连接错误回调：清状态，避免悬挂指针。
```

```
353 static err_t pd_tcp_accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
356 if (err!=ERR_OK || g_client!=NULL) { tcp_close(pcb); return ERR_ABRT; }
→ 单连接限制：已有客户端则拒绝新连接（返回 ERR_ABRT）。
360 g_client = pcb; ... tcp_recv(pcb, pd_tcp_recv_cb); tcp_err(pcb, pd_tcp_err_cb);
365 tcp_reply("PD_ACQ TCP V2 READY; type HELP");
→ 接受连接、注册回调、发欢迎行。
```

```
369 static int tcp_server_start(void)
374 listener = tcp_new_ip_type(IPADDR_TYPE_ANY);
376 err = tcp_bind(listener, IP_ANY_TYPE, PD_TCP_PORT);
378 listener = tcp_listen(listener);
380 tcp_accept(listener, pd_tcp_accept_cb);
→ 标准 lwIP RAW 监听建立：建 pcb → 绑端口 → 转监听 → 注册 accept 回调。
```

```
384 static void pd_tcp_network_poll(void)
386 if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag=0; }
390 if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag=0; }
394 xemacif_input(&echo_netif);   /* 把 EMAC 收到的包送入 lwIP */
395 tcp_transfer_pump();           /* 驱动在途 GET 分块发送 */
→ 网络轮询：消费定时器标志、收包、泵送数据。此函数即被引擎的 DMA 等待循环以 PD_ACQ_NETWORK_POLL 调用，也由 main 主循环直接调用（第 443 行）。
```

```
398 int main(void)
402 unsigned char mac[6] = { PD_TCP_MAC0..5 };
413 init_platform();
414 IP4_ADDR(&ipaddr,192,168,1,10); ... netmask 255.255.255.0; gw 192.168.1.1;
417 lwip_init();
418 xemac_add(&echo_netif,&ipaddr,&netmask,&gw,mac,PLATFORM_EMAC_BASEADDR);
423 netif_set_default(&echo_netif);
425 platform_enable_interrupts();   /* #ifndef SDT 才使能中断 */
427 netif_set_up(&echo_netif);
→ 标准 lwIP 裸机初始化：设静态 IP、加 EMAC、设默认接口、使能中断、接口 up。xemac_add 失败则 TCP_ACQ_FAIL 并 WFI。
431~437 DMA 初始化（同引擎）：Lookup/CfgInit/HasSg 守卫/IntrDisable/Reset。
438 if (tcp_server_start()!=0) halt_failure("TCP listen failed");
440 xil_printf("TCP_ACQ_READY: connect 192.168.1.10:%u and send HELP\r\n");
441 for (;;) {
442     uart_poll();               /* UART 仍可用作调试口 */
443     pd_tcp_network_poll();     /* 网络轮询（含 GET 泵送） */
445     if (!g_running) { ... START 处理（同引擎，仅多了 net 钩子已自动生效） ... }
457     if (g_stop_request || !receive_one_packet()) { ... clean_stop ... }
467     archive_new_snapshot(...); ++packets; ... ACQ_SERVICE_PASS ...
→ 主循环与第九节 V1 引擎完全一致，唯一增量是每轮多调 pd_tcp_network_poll（且引擎内部 DMA 等待循环也会调它）。即“同一采集状态机 + TCP/UART 双控制面”。
```

### 易错点小结（`pd_acquisition_tcp_service_main.c`）

1. **`.inc` 必须保持、不可改 `.c`**：`pd_acquisition_service.inc` 与 `.c` 逐字节相同，靠 `#ifndef PD_ACQ_NO_MAIN` 让 TCP 版跳过引擎 main；若误把 `.inc` 改名 `.c` 加入编译，会与本文件 main 冲突（README 已强调）。
2. **GET 仅允许空闲时**：`g_running` 时拒绝下载，防止读到在途/被覆盖归档；这是数据安全边界，勿删。
3. **下载期间忽略新命令**：`g_transfer.active` 时 `tcp_execute_command` 直接 return，客户端须先读完裸 payload 再发令，否则会字节交错。
4. **每次 GET 限 16 KiB、每次 tcp_write 1 KiB**：既防 lwIP 缓冲耗尽，也避免回调阻塞；完整 3.12 MB 快照需上位机分块循环读取（见 `pd_tcp_download_record.ps1`）。
5. **从 DDR 归档取数前必须 `Xil_DCacheInvalidateRange`**（第 133、249 行）：否则可能发送陈旧 cache 内容——这是裸机 DMA/CPU 共享内存的经典坑。
6. **单连接限制**：`g_client` 仅一个，第二连接被拒；多客户端需改此模型。
7. **TCP 与 UART 共享同一 `g_acq` 状态机**：两边命令等价，但 GET 大块下载走 TCP 专属泵送，UART 不提供裸数据下载（带宽不足，说明文档已说明）。

---

## 全局演进关系总览（回答各文件“谁被谁取代”）

按时间/成熟度排序：

1. `dma_s2mm_dds_test.c`（最早，128 B 固定 BTT，字段 10-bit，Busy 判完成）→ **缺陷版**，仅历史参考。
2. `dma_s2mm_probe.c`（v1，BTT=65528，测包长分布，证实 LENGTH=已写字节数）→ 取代 dds_test。
3. `dma_s2mm_probe_v2.c`（=v1 + 流一致性核算，判别速率驱动 vs 缓冲驱动）→ v1 的功能增强。
4. `dma_s2mm_probe_v3.c`（=v2 + 有界运行 + park）→ v2 的“可安全反复上板”终态。
5. `pd_feature_dma_s2mm_smoke.c`（特征→DMA 有界冒烟，FEATURE_DMA_S2MM_PASS）→ 与 probe 系列并列的验收冒烟。
6. `pd_filter_axil_smoke.c`（滤波器 AXI-Lite 可达性）→ `pd_filter_apply.c` 的前置 sanity。
7. `pd_filter_apply.c`（IIR 系数安全下发，FILTER_APPLY_VERIFY_PASS）→ 标定专属，不被采集服务取代，反被采集服务“前置调用”。
8. `pd_capture_service.c`（首个集成服务，CAPTURE_SERVICE_PASS，只取快照首末字）→ 较早集成验证。
9. `pd_acquisition_service.c`（V1 整体式服务，含环形归档 + UART 控制，ACQ_SERVICE_PASS）→ pd_capture_service 的“产品化”。
10. `pd_acquisition_tcp_service_main.c`（=V1 引擎 + lwIP TCP V2 控制/下载）→ V1 的网络化版本；其引擎即 `pd_acquisition_service.inc`（= `.c`）。

**当前活跃服务是 `sw/ps_service/`**（`pd_acquisition_core.c` + `tcp/pd_tcp_service.c`），故 A15 全部 10 个文件均属“早期/探针/辅助”范畴，用于理解演进脉络与单点验证，不直接等于现网服务。

---

## 本卷发现的三个最重要问题（跨文件）

1. **`XAxiDma_Busy()` 误用**：`dma_s2mm_dds_test.c`、`pd_feature_dma_s2mm_smoke.c`、`pd_capture_service.c`、`pd_acquisition_service.c`（`receive_one_packet`）在 DMA 等待循环里仍以 `XAxiDma_Busy()` 作退出条件。其驱动实现只看 Idle 位，Halted 时永久返 TRUE，导致出错后自旋满 POLL_LIMIT 才被后续 DMASR 检查兜底（甚至 dds_test 直接把错误淹没）。probe v1 已改为直接读 DMASR(Idle|Halted|Error) 立即跳出，建议采集服务同步修复。

2. **`pd_filter_apply.c` 的“回读一致 ≠ 已 APPLY”陷阱**：`check()` 仅验证影子寄存器写回一致；真正生效判据是 APPLY 后 `FSTATUS[1]`（coef_dirty）由 1 变 0（第 72 行）。若只依赖回读会误判“系数已生效”。该判据已在源码正确实现，但属极易被忽略的隐性契约。

3. **两个 PS 归档区首尾紧邻无保护间隙 + `SNAP_TRIG_CTRL` bit8 / `SLOT_CTRL` bit12 位语义未具名**：快照归档 `0x2400_0000~0x2700_0000` 与事件归档 `0x2700_0000~` 直接相邻，安全性全靠 `bytes <= 12 MiB` 校验；且自动快照触发控制里的 bit8、槽控制里的 bit12 在源码中没有具名宏、仅有字面量/SLOT_FULL 复用，属 PL 契约未在本工程展开之处，改 PL 或扩快照长度时需特别警惕。

---

## 本卷到此为止
