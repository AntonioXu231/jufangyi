# `pd_feature` AXI-DMA S2MM 冒烟测试 · 逐行注释

> 面向：PS 侧裸机 C（Vitis 2024.1 standalone）。
> 目的：读懂每一行在干什么，并补上 AXI-Stream / DMA / Cache 的背景知识。
> **本注释同时做了一件核验**：把代码里的寄存器偏移、DMASR 位、**事件包位域**逐条对照 RTL 单点定义
> （`pd_defines.vh`、`pd_feature_core.v`、`pd_ddr_axil.v`），结论见文末 §8。

---

## 0. 这段代码在整条链路里的位置

```
  [AD9226/DDS] → [IIR/特征核 pd_feature_core] → [4 通道事件 FIFO pd_axis_fifo]
                                                        │
                                            [仲裁 pd_axis_arb]（4 路合 1 路）
                                                        │
                                            pd_feature_0/m_axis（64bit AXI4-Stream）
                                                        │
                                              axi_dma_0 的 S2MM 通道（Simple 模式）
                                                        │  AXI 主口 → HP0
                                                        ▼
                                             PS DDR（接收缓冲 0x0110_0000）
                                                        │
                                                   本程序轮询读回、解析
```

**S2MM = Stream to Memory-Mapped**，即"从流搬到内存"。它是 AXI DMA 的一个方向；另一个方向 MM2S 是"内存搬出到流"。本测试只用 S2MM。

**为什么不用中断**：BD 里 S2MM 的中断已接 `IRQ_F2P[2]`，但 BSP 里没有导出好用的中断号宏；轮询能把"中断配错"这个变量先隔离掉，专注验数据通路。

**A 档案与 B 档案**：本文件是 **A 档案**（`pd_feature_0/m_axis` 事件流 → DMA → DDR）。

---

## 1. 头文件

```c
#include "xparameters.h"   // BSP 自动生成：所有外设的基地址宏、器件 ID
#include "xaxidma.h"       // AXI DMA 驱动（Xilinx 官方，PG021）
#include "xil_cache.h"     // Xil_DCacheFlushRange / InvalidateRange
#include "xil_io.h"        // Xil_In32 / Xil_Out32（带内存屏障的寄存器读写）
#include "xil_printf.h"    // 精简版 printf（可走 UART，体积小）
#include "xil_types.h"     // u8/u16/u32/u64/s16 等
#include "xstatus.h"       // XST_SUCCESS / XST_FAILURE
```

**背景**：`xparameters.h` 是 Vitis 根据 `.xsa` 生成的，里面的宏名会随 BSP 版本变化（见下面 §2 的条件编译）。

---

## 2. 编译期分支：找到外设基地址

```c
#if defined(XPAR_XAXIDMA_0_BASEADDR)
# define DMA_BASE        ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG  XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
# define DMA_BASE        ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG  XPAR_AXIDMA_0_DEVICE_ID
#else
# error "No AXI-DMA instance in xparameters.h: regenerate the Vitis platform from the current XSA."
#endif
```

- **两种 BSP 风格**：
  - **SDT（System Device Tree，Vitis 2023+ 新流程）**：宏是 `XPAR_<periph>_BASEADDR`，驱动查找用**基地址**。
  - **经典 BSP（老流程）**：宏是 `XPAR_<periph>_BASEADDR` + `..._DEVICE_ID`，驱动查找用**器件号**。
- `XAxiDma_LookupConfig()` 的参数在两种流程下含义不同，所以这里分开定义 `DMA_LOOKUP_ARG`。
- `#error` 是编译期兜底：宏找不到就直接编译失败，而不是运行时跑出错地址（这比 `fail_stop` 更早、更硬）。

```c
#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
# define PS_DDR_BASE XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
# define PS_DDR_BASE XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
# error "No PS DDR base macro in xparameters.h: inspect the generated BSP."
#endif
```

- 同样是为了兼容两代 BSP 的 DDR 宏名。Zynq-7000 上 PS DDR 通常从 `0x0010_0000` 起（低 1 MB 保留给 OCM/寄存器）。

---

## 3. 关键宏：缓冲地址、长度、上限

```c
/* Keep this below the application image and outside the raw ring / slot areas. */
#define RX_BUFFER_BASE       ((UINTPTR)PS_DDR_BASE + 0x01000000U)
```
- 接收缓冲 = `PS_DDR_BASE + 16 MB`（典型为 `0x0110_0000`）。
- **为什么是这里**：要避开三块区域 ——
  1. 程序镜像与堆栈（`PS_DDR_BASE` 附近）；
  2. 原始样点环 `0x1000_2000 ~ 0x1800_0000`（`pd_ddr_defines.vh:80-81`）；
  3. 四槽快照区 `0x2000_1000 ~ 0x2300_1000`（`:134-139`）。
  `0x0110_0000` 全部避开。

```c
#define S2MM_MAX_BTT         65528U  /* 16-bit BTT, aligned down to 8 bytes */
```
- **BTT = Bytes To Transfer**，即 S2MM 接收的上限字节数。`65528 = 0xFFFF - 7 = 0xFFF8`，是"16 位能表示的最大 8 字节对齐值"。
- **为什么必须够大**：DMA 收满 BTT 就会结束，**不用等 TLAST**。若 BTT 小于实际包长，包会被截断并报内部错误（历史教训：`DMASR=0x5011` 就是 `BTT=128 < 包长 8224` 引起的）。
- 设成远大于最长包，DMA 就会**老老实实等 TLAST**，从而"一次传输 = 一个完整包"。

```c
#define PACKET_LIMIT         128U        // 本轮收 128 个包
#define POLL_LIMIT           20000000U   // 单包等待的上限轮数（防死等）
#define REPORT_PERIOD        16U         // 每 16 个包打印一次详情（前 2 个必打）
```

---

## 4. DMA 寄存器与 DMASR 位（PG021）

```c
#define S2MM_DMACR_OFFSET    0x30U   // S2MM 控制寄存器（中断使能、复位、IRQ 阈值…）
#define S2MM_DMASR_OFFSET    0x34U   // S2MM 状态寄存器（只读，W1C 清标志）
#define S2MM_LENGTH_OFFSET   0x58U   // S2MM 长度/启动寄存器
```
- 这三个偏移是 PG021 表里的固定值（S2MM 偏移 = MM2S 偏移 + 0x30）。**已核对：正确。**

```c
#define DMASR_HALTED         0x00000001U  // bit0  1=DMA 被停住（复位中或出错后自锁）
#define DMASR_IDLE           0x00000002U  // bit1  1=空闲（没有正在进行的传输）
#define DMASR_DMA_INT_ERR    0x00000010U  // bit4  内部错误（最常见：长度/包不匹配）
#define DMASR_DMA_SLV_ERR    0x00000020U  // bit5  从机错误（AXI 从端回答错误）
#define DMASR_DMA_DEC_ERR    0x00000040U  // bit6  译码错误（访问了非法地址）
#define DMASR_IOC_IRQ        0x00001000U  // bit12 传输完成标志
#define DMASR_DLY_IRQ        0x00002000U  // bit13 延迟中断
#define DMASR_ERR_IRQ        0x00004000U  // bit14 错误中断
#define DMASR_ERROR_MASK     (DMASR_DMA_INT_ERR | DMASR_DMA_SLV_ERR | \
                              DMASR_DMA_DEC_ERR | DMASR_ERR_IRQ)
```
- **判断顺序永远是**：先看有没有错（bit4/5/6/14），再看是不是 Halted，最后确认 Idle。
- 这些位是 **W1C**（写 1 清零）——`XAxiDma_IntrAckIrq` 就是干这个的。

```c
#define EV_TYPE_PEAK         0x00U   // 峰值事件包
#define EV_TYPE_CYCLE        0x01U   // 周期统计包（同时也是帧尾）
```
- **已核对**：与 `pd_defines.vh:23-24`（`PD_EV_PEAK=8'h00`、`PD_EV_CYCLE=8'h01`）一致。

---

## 5. 全局与工具函数

```c
static XAxiDma g_dma;   // DMA 驱动实例；只在 main 里初始化，后续传给所有驱动 API
```

```c
static inline u32 dma_reg_read(u32 offset)
{
    return Xil_In32(DMA_BASE + offset);
}
```
- **为什么不直接用 `XAxiDma_ReadReg`**：驱动宏有时要求传入实例；直接基地址 + 偏移更直观，也方便打印原始寄存器值。
- `Xil_In32` 内部是**带 `dmb` 内存屏障的 volatile 读**，保证"真的去读了寄存器"，不会被编译器优化掉。

---

## 6. `park_after_failure` —— 失败即停

```c
static void park_after_failure(const char *reason)
{
    xil_printf("FEATURE_DMA_S2MM_FAIL: %s\r\n", reason);   // 打印失败点（唯一的证据来源）
    xil_printf("DMACR=%08x DMASR=%08x LENGTH=%u\r\n",       // 现场三件套：控制/状态/长度
               dma_reg_read(S2MM_DMACR_OFFSET),
               dma_reg_read(S2MM_DMASR_OFFSET),
               dma_reg_read(S2MM_LENGTH_OFFSET));
    for (;;) {
        __asm__ volatile ("wfi");                           // WFI：CPU 休眠，直到中断
    }
}
```
- **设计意图**：失败后**不重试、不掩盖**，把现场冻结。这正是本项目"真错误不得被掩盖"的原则。
- `wfi`(Wait For Interrupt) 让 CPU 停下。因为本程序关掉了中断，所以它会一直停住 —— 这是**故意**的：方便你在 Vitis 里"halt 再下载新 ELF"。
- `volatile` 防止编译器把空循环优化掉。

---

## 7. `print_event_word` —— 把一个 64 位事件字解析成人话

```c
static void print_event_word(u32 index, u64 word)
{
    u32 hi = (u32)(word >> 32);   // 高 32 位 → 对应硬件 tdata[63:32]
    u32 lo = (u32)word;           // 低 32 位 → 对应硬件 tdata[31:0]
    u32 type = (hi >> 24) & 0xffU;  // 类型恒在 [63:56]
```
- **背景**：AXI-Stream 是**字节流**，DMA 按小端把每个 64bit beat 写进 DDR。PS 用 `u64` 读回来，位序天然与 RTL 的 `tdata[63:0]` 一一对应，所以 `word>>56` 就是 `tdata[63:56]`。
- `type` 先取出来，**下面按 type 走两套完全不同的解码**——这是本文件的重点，两种包型的字段位置不同。

### 7.1 峰值包（type = 0x00）

```c
    if (type == EV_TYPE_PEAK) {
        u32 phase = ((hi & 0xffU) << 4) | ((lo >> 28) & 0x0fU);  // [39:28]，12 位
        u32 ch    = (lo >> 25) & 0x3U;                          // [26:25]，2 位
        u32 pol   = (lo >> 27) & 0x1U;                          // [27]，1 位
        s16 q     = (s16)((hi >> 8) & 0xffffU);                 // [55:40]，16 位有符号
```
**逐位解释**（`hi` = [63:32]，`lo` = [31:0]）：
- `phase`：`hi&0xff` 取 [39:32]，左移 4 位放到 [43:36]；再或上 `lo>>28` 的 [31:28]；
  合起来正好是 **[39:28] 的 12 位**。→ 这是**相位窗号**（窗数上限由 `PD_PH_W=12` 决定，最大 4096 窗）。
- `ch`：`lo>>25 & 3` → **[26:25]**，通道号 0~3。
- `pol`：`lo>>27 & 1` → **[27]**，极性。RTL 里写的是 `~q0_pol_r`，即**包里 1=正、0=负**（`pd_feature_core.v:556`）。
- `q`：`hi>>8 & 0xffff` → **[55:40]**，16 位有符号，**Q8.8 格式的视在电荷量 pC**。强转 `s16` 才能正确显示负数。

```c
        xil_printf("  %02u PEAK  raw=%08x_%08x q=%d phase=%u pol=%u ch=%u\r\n",
                   index, hi, lo, (s32)q, phase, pol, ch);
```
- 先打 `raw=hi_lo` —— **原始字一定要打**，解析错了还能靠它回算。

### 7.2 周期统计包（type = 0x01，也是帧尾）

```c
    } else if (type == EV_TYPE_CYCLE) {
        u32 cycle_idx   = hi & 0x00ffffffU;      // [55:32]，24 位
        u32 ch          = (lo >> 30) & 0x3U;     // [31:30]，2 位
        u32 event_count = (lo >> 16) & 0x3fffU;  // [29:16]，14 位
        xil_printf("  %02u CYCLE raw=%08x_%08x idx=%u ch=%u n=%u qmax=%u\r\n",
                   index, hi, lo, cycle_idx, ch, event_count, lo & 0xffffU);
```
- **注意：周期包的 `ch` 在 [31:30]，不是峰值包的 [26:25]**。两包型通道位置不同，必须按 type 分派（RTL `pd_feature_core.v:574-576` 已经明确写了这条警告）。
- `cycle_idx` 是**时间戳**（`TS_W=24`，单位 ADC 采样周期），`event_count` 是该周期内的事件数，`lo&0xffff` 是该周期最大电荷（Q8.8）。

### 7.3 其它

```c
    } else {
        xil_printf("  %02u UNKNOWN raw=%08x_%08x type=%02x\r\n", index, hi, lo, type);
    }
```
- 未识别类型也打出来 —— **不静默丢弃**，方便发现"位序错了导致 type 乱"的问题。

---

## 8. `inspect_packet` —— 校验"一个传输 = 一个完整包"

```c
static int inspect_packet(u32 packet_index, u32 bytes,
                          u32 *peak_words, u32 *cycle_words)
{
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;   // 按 64bit 字视图访问缓冲
    u32 words = bytes / 8U;                              // 字节数 → 字数
    u32 i;
    u32 last_type;
```
- `volatile` 很关键：缓冲是**DMA 写的**，不是 CPU 写的；不加 `volatile` 编译器可能把重复读取合并/缓存。

```c
    if ((bytes == 0U) || (bytes > S2MM_MAX_BTT) || ((bytes & 7U) != 0U))
        return XST_FAILURE;
```
- 三条合理性检查：非 0、不超上限、**8 字节对齐**（AXIS 数据宽度 64bit，字节数必是 8 的倍数）。

```c
    for (i = 0U; i < words; ++i) {
        u32 type = ((u32)(rx[i] >> 56)) & 0xffU;   // 每个字的 [63:56] 就是类型
        if (type == EV_TYPE_PEAK)      (*peak_words)++;
        else if (type == EV_TYPE_CYCLE) (*cycle_words)++;
    }
```
- 用**指针**累加计数（`*peak_words++`），这样跨包累计。

```c
    last_type = ((u32)(rx[words - 1U] >> 56)) & 0xffU;
    if (last_type != EV_TYPE_CYCLE)
        return XST_FAILURE;
```
- **本测试的核心判据**：最后一拍必须是周期统计包。因为 RTL 只在周期包上拉 `ev_tlast`（`pd_feature_core.v:844`），
  而 S2MM Simple 模式遇到 TLAST 就结束传输 ⇒ "传输结束"与"帧尾到达"**互为证明**。
  - 若最后一拍不是 cycle 包 ⇒ 要么 DMA 是因"收满 BTT"结束的（包被截断），要么位序错了。

```c
    if ((packet_index < 2U) || ((packet_index % REPORT_PERIOD) == 0U)) { ... }
```
- 只打印前 2 个包，之后每 16 个打一次，避免刷屏。

---

## 9. `main`

```c
    xil_printf("\r\n--- pd_feature AXI-DMA S2MM smoke test ---\r\n");
    xil_printf("DMA=0x%08x RX=0x%08x BTT=%u packets=%u\r\n", ...);
```
- 先把关键地址打出来，便于和 `.xsa` 的地图对照。

```c
    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL) park_after_failure("XAxiDma_LookupConfig returned NULL");
    status = XAxiDma_CfgInitialize(&g_dma, cfg);
    if (status != XST_SUCCESS) park_after_failure("XAxiDma_CfgInitialize failed");
    if (XAxiDma_HasSg(&g_dma)) park_after_failure("hardware is Scatter-Gather mode, expected Simple mode");
```
- 标准三步：**查找配置 → 初始化 → 检查模式**。
- `HasSg` 为真是"硬件被配成了散列/聚集（Scatter-Gather）模式"，那要用 BD 环 + 描述符，本程序按 Simple 写，所以直接失败退出。

```c
    XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);  // 关 S2MM 中断 → 纯轮询
    XAxiDma_Reset(&g_dma);                                                     // 软复位 DMA
    while (!XAxiDma_ResetIsDone(&g_dma)) { }                                   // 等复位完成
```
- **复位必须等完**再武装传输，否则可能残留旧状态。

```c
    for (packet = 0U; packet < PACKET_LIMIT; ++packet) {
        u32 i; u32 dmasr; u32 bytes;

        Xil_DCacheFlushRange(RX_BUFFER_BASE, S2MM_MAX_BTT);      // 把 CPU 侧脏行写回
        Xil_DCacheInvalidateRange(RX_BUFFER_BASE, S2MM_MAX_BTT); // 丢弃 CPU 缓存，强制从 DDR 重读
        XAxiDma_IntrAckIrq(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA); // W1C 清状态
```
- **Cache 背景（Zynq 必踩）**：PS 有 L1/L2 Cache，DMA 直接写 DDR，**不经过 Cache**。
  所以 CPU 若只看缓存，会读到旧数据 ⇒ 必须 `Invalidate`。
  这里先 Flush 再 Invalidate，是为了清掉上一轮遗留的脏行，属于保守写法。

```c
        status = XAxiDma_SimpleTransfer(&g_dma, RX_BUFFER_BASE, S2MM_MAX_BTT, XAXIDMA_DEVICE_TO_DMA);
        if (status != XST_SUCCESS) park_after_failure("XAxiDma_SimpleTransfer failed");
```
- 这一步把 `S2MM_LENGTH = S2MM_MAX_BTT` 写下去，**从此 S2MM 开始拉高 TREADY 准备收数**。
- `XAXIDMA_DEVICE_TO_DMA` 表示方向是 S2MM。

```c
        for (i = 0U; i < POLL_LIMIT; ++i) {
            if (!XAxiDma_Busy(&g_dma, XAXIDMA_DEVICE_TO_DMA)) break;
        }
```
- **轮询等传输结束**。`XAxiDma_Busy` 实质是看 DMASR 的 Idle/Halted 位。
- 循环结束后：`i < POLL_LIMIT` 表示正常退出；`i == POLL_LIMIT` 表示超时。

```c
        dmasr = dma_reg_read(S2MM_DMASR_OFFSET);
        bytes = dma_reg_read(S2MM_LENGTH_OFFSET);
        if ((i == POLL_LIMIT) || (dmasr & DMASR_ERROR_MASK) ||
            (dmasr & DMASR_HALTED) || !(dmasr & DMASR_IDLE))
            park_after_failure("S2MM timeout, halt, or AXI error");
```
- **四重判据一次判完**：超时 / 有错误位 / 被停住 / 不空闲。
- 原理：传输结束后，S2MM 的 LENGTH 寄存器被硬件改写为**本次实际接收的字节数**，所以 `bytes` 就是包长。

```c
        if ((bytes == 0U) || (bytes > S2MM_MAX_BTT) || ((bytes & 7U) != 0U))
            park_after_failure("S2MM returned an invalid byte count");
        Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
        if (inspect_packet(packet, bytes, &peak_words, &cycle_words) != XST_SUCCESS)
            park_after_failure("event packet contract failed");
```
- 长度合理性检查 → 按**实际长度**失效 Cache（只失效有效区域）→ 解析校验。

```c
        if (bytes < bytes_min) bytes_min = bytes;
        if (bytes > bytes_max) bytes_max = bytes;
        bytes_total += bytes;
    }
```
- 统计包长分布。**变长包**是这套设计的正常形态（峰值事件数随工况变化），所以用 min/max/avg 看形状。

```c
    xil_printf("FEATURE_DMA_S2MM_PASS packets=%u peak_words=%u cycle_words=%u ", ...);
    xil_printf("bytes_min=%u bytes_max=%u bytes_avg=%u\r\n", ...);
    xil_printf("STOP: DMA is idle; halt CPU before downloading another ELF.\r\n");
    for (;;) { __asm__ volatile ("wfi"); }
```
- **收尾姿态**：不再武装任何传输（DMA 处于 Idle），CPU 停在 `wfi`。
  ⇒ 此时直接 halt 下载下一个 ELF 是安全的，不会有一半的 DMA 传输在跑。

---

## 10. 核验结果（我逐条对照了 RTL）

### ✅ 正确的部分

| 项 | 代码 | RTL 依据 | 结论 |
|---|---|---|---|
| DMA 寄存器偏移 `0x30/0x34/0x58` | §4 | PG021 | ✅ |
| DMASR 位 `1/2/4/5/6/12/13/14` | §4 | PG021 | ✅ |
| 事件包类型 `0x00/0x01` | §4 | `pd_defines.vh:23-24` | ✅ |
| **PEAK 包位域**：`type[63:56]` / `q[55:40]` / `phase[39:28]` / `pol[27]` / `ch[26:25]` | §7.1 | `pd_feature_core.v:562-567` + `pd_defines.vh:33-36`（`8+16+12+1+2+25=64`） | ✅ |
| **CYCLE 包位域**：`type[63:56]` / `idx[55:32]` / `ch[31:30]` / `n[29:16]` / `qmax[15:0]` | §7.2 | `pd_feature_core.v:577-578`（`8+24+2+14+16=64`） | ✅ |
| "帧尾 = 周期包" | §8 | `pd_feature_core.v:844` `ev_tlast<=1'b1`（周期统计包=帧尾） | ✅ |
| `i == POLL_LIMIT` 判超时 | §9 | 循环从 0 到 POLL_LIMIT-1，`break` 才小于 | ✅ |

### ⚠️ 两点提醒（不是错，但要知道）

1. **RTL 里的注释是旧的、代码是对的。**
   `pd_feature_core.v:555,557` 仍写着 `[39:30] phase / [28:27] ch_id / [26:0] evt_seq`，
   那是 `PD_PH_FIELD_W=10` 时的位置。实际 `PD_PH_FIELD_W=12`（`pd_defines.vh:36`），
   真实位置是 `[39:28] / [27] / [26:25] / [24:0]`。**本 C 文件按 12 位解码，与硬件一致**；
   错的是 RTL 注释。建议顺手把 `:552-558` 的注释改掉（属"单点定义"纪律，`pd_defines.vh` 才是准）。

2. **`bytes` 建议加掩码**：官方例程读接收长度是 `... & 0x7FFFFF`。
   当前直接用 `dma_reg_read(S2MM_LENGTH_OFFSET)`，若该寄存器高位有非长度位，长度会被污染。
   建议改成 `bytes = dma_reg_read(S2MM_LENGTH_OFFSET) & 0x7FFFFF;`。

### ℹ️ 测试覆盖面的边界

- 本测试证明的是**"事件流 → S2MM → DDR → PS 读回"这条搬运链路与包契约**；
  它**不校验事件数值本身的正确性**（q/phase/ch 只打印，不做期望值比对）。
  数值正确性要靠特征核的模块级仿真或已知激励比对。
- Simple 模式是"**一次传输、一次武装**"：两次传输之间 S2MM 不拉 TREADY，上游 FIFO 会积压甚至溢出。
  所以"收到 128 个包"≠"源正好发了 128 个包"。这是冒烟测试可接受的近似，但做正式驱动时要改成**常驻接收**（或 SG 模式）。
