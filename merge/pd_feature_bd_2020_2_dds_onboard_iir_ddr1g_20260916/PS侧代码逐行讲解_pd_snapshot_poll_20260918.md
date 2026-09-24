# PS 侧代码逐行讲解 —— `pd_snapshot_poll.c`

讲解对象：`F:\ps1\app_component\src\pd_snapshot_poll.c`（223 行）
Vitis workspace：`F:\ps1`｜平台：Zynq-7020（PS = 双 Cortex-A9 @666.67 MHz）
配套硬件：`pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`
写这份文档时的工程状态：UART0 已配到 MIO 14/15，BSP stdout 已切到 `XPAR_STDIN_IS_UARTPS`（`0xE0000000`）

> **怎么用这份文档**：§1 讲"PS 到底干嘛"，是概念；§3 是逐行代码注释，遇到不懂的行号回来查；
> §4 是五个必须吃透的点（W1P、Cache 一致性、轮询 vs 中断……），这几个不搞懂，代码看懂了也会踩坑。

---

# §1 PS 是干嘛的

## 1.1 先建立一个最粗的图

Zynq-7020 是一颗"把 CPU 和 FPGA 焊在一起"的芯片。里面有两块性质完全不同的东西：

```
┌──────────────────────── Zynq-7020 (xc7z020) ────────────────────────┐
│                                                                     │
│   PS  (Processing System)              PL  (Programmable Logic)      │
│   = 硬核，出厂就固定好了                = FPGA 可编程逻辑              │
│   ├─ 双核 ARM Cortex-A9 @667 MHz       ├─ LUT / 触发器 / DSP48        │
│   ├─ DDR3 控制器                       ├─ Block RAM                  │
│   ├─ UART / I2C / SPI / SD / USB       ├─ 你写的 Verilog 全部在这一侧  │
│   ├─ 中断控制器 GIC                    ├─ 时钟 MMCM / PLL              │
│   └─ 内存管理单元 MMU（含 Cache）        └─ 没有"操作系统"概念            │
│                                                                     │
│         ←── 两者靠 AXI 总线互连（GP0/GP1/HP0/HP1）──→                  │
└─────────────────────────────────────────────────────────────────────┘
```

**一句话区分**：PS 是"会做算术、会循环、会判断，但一次只能干一件事"的 CPU；PL 是"能同时并行做几百件事，但不会自己思考"的逻辑电路。

## 1.2 为什么这个项目非要分两块

你做的局放检测，几个硬指标互相打架：

| 需求 | 适合谁 | 理由 |
|---|---|---|
| 4 通道 × 12 bit × 26 MSPS 持续收数 | **PL** | 每秒 2600 万个采样点，CPU 光"收"就来不及，而且时序必须一个时钟都不差 |
| 采样域 → 处理域的跨时钟域（CDC） | **PL** | CPU 根本没有"时钟域"这个概念 |
| 环形写 DDR、冻结、快照拷贝 | **PL** | 毫秒级、不可延迟；且 DDR 挂在 PS 上，PL 通过 HP 口直接写更快 |
| 实时判决（阈值、峰值、相位窗、PRPD 计数） | **PL** | 必须和采样时钟同步，一个周期都不能漏 |
| **FFT / 选频 / 频谱统计** | **PS** | 你们的实测结论是：四路 FFT 塞进 PL 会导致 xc7z020 **放置失败**（Slice 需 7,232 > 7,205）。放 PS 上慢慢算完全来得及 |
| 上位机通讯（TCP/UDP）、文件存储、设备状态机 | **PS** | 涉及协议栈、字符串、状态机，CPU 的强项 |
| 参数配置（阈值、滤波系数、快照策略） | **PS 算，PL 用** | 系数在 PS 侧离线算好，通过 AXI-Lite 下发给 PL |

**核心分工原则**（记住这一条就够）：
> **和"时间"绑死的放 PL，和"逻辑/算法灵活性"绑死的放 PS。**

判决支路留在 PL，是因为它必须跟着 26 MSPS 的采样节拍走；
分析支路（FFT）放 PS，是因为它允许几百毫秒的延迟，而 CPU 算起来更省芯片资源。

## 1.3 PS 在本工程里的职责清单

按你们计划书 §6.2 的分解，PS 要做六件事：

| # | 功能 | 状态 |
|---|---|---|
| 1 | AXI DMA S2MM 裸机探测 | 已完成（保留为驱动参考） |
| 2 | DDR 槽读取 / 锁定 / 释放 | **← 本程序在做这一条** |
| 3 | 快照 IRQ 服务 | 未开始（本程序先用轮询替代） |
| 4 | **IIR 系数计算与下发** | 未开始（`pd_filter_apply` 已做过一轮下发验证） |
| 5 | **FFT、频谱与选频** | 未开始（PS 端批处理） |
| 6 | 通讯和设备状态机 | 未开始 |

## 1.4 这个程序本身：它不是产品代码

`pd_snapshot_poll.c` 的定位是**一次"契约自检"**——用最小代价证明三件事：

1. PS 能不能正确**读**到 PL 的 AXI-Lite 寄存器（地址映射对不对）；
2. PS 能不能正确**配**自动快照（使能环采、使能四槽自动拷贝、武装事件触发）；
3. PL 真的自动完成一次快照后，PS 能不能**发现并安全地把数据取出来**（锁定 → 读 → 释放 → 重新武装）。

它**不做**：FFT、滤波、协议、错误恢复。跑通它，只证明"PL/PS 的数据契约通了"，不证明算法对。

---

# §2 这个程序在整条链路里的位置

```
DDS/ADC 源(26 MSPS) ──▶ pd_ddr_0 ──┬─▶ 环形缓冲写 DDR（存档支路，存原始样点）
                                    └─▶ o_feat_data ──▶ pd_filter_0 ──▶ pd_feature_0
                                                                             │
                                                             事件被 FIFO 接收
                                                                             │
                                                    o_event_accept ──▶ pd_snapshot_trigger
                                                                             │
                                                                      自动产生冻结触发
                                                                             │
                                                        周期边界冻结 → 拷到四槽中某一槽
                                                                             │
                                                        SLOT_STATUS[19] snapshot_ready ──┐
                                                                                          │
        ┌──────────────────────── 本程序在这里工作 ────────────────────────────────────────┘
        │  轮询 SLOT_STATUS，看到 ready
        │  → 读槽描述符（BASE / LEN / SEQ / FLAGS）
        │  → 锁定这个槽（防止 PL 复用它）
        │  → cache 失效 + 读出首末字
        │  → 释放槽
        │  → 发 freeze_resume，让 PL 重新武装，准备下一次
        └─▶ 打印 SNAPSHOT_POLL_PASS
```

**注意**：数据本体（那一整槽 12 MiB 的原始样点）都在 **DDR** 里，PS 只是通过 AXI-Lite 读"描述符"知道"数据放在哪、多长"。
真正要拿数据分析时，读的是 DDR 地址（`base`），不是寄存器。本程序只读首末两个字证明"能读通"，没有搬全量数据。

---

# §3 逐行注释

## 3.1 文件头注释（L1–L13）

```c
/*
 * Bounded PS-side automatic snapshot smoke test.
 *
 * This test intentionally polls pd_ddr_0 instead of installing a GIC handler.
 * The BD already routes pd_ddr_0/irq to IRQ_F2P[2], but the generated BSP does
 * not expose a named pd_ddr interrupt macro yet. Polling therefore verifies the
 * complete PL/DDR/AXI-Lite contract without guessing an interrupt ID.
 *
 * Required hardware state:
 *   - current pd_feature_bd_wrapper.bit is programmed
 *   - pd_filter_apply has already printed FILTER_APPLY_VERIFY_PASS
 *   - DDS/event source is running and can produce at least one accepted event
 */
```

| 行 | 讲解 |
|---|---|
| L2 | `Bounded`（有界的）= 这个测试有超时、会自己停，不会死等。**裸机程序必须"有界"**，否则卡住只能靠复位。 |
| L4 | **故意用轮询，不用中断**。这是个工程判断，值得学：轮询丑但确定，中断优雅但要依赖中断号。 |
| L5–L6 | 我核实过 BD：`pd_feature_0/irq → xlconcat_0/In0`、`axi_dma_0/s2mm_introut → xlconcat_0/In1`、**`pd_ddr_0/irq → xlconcat_0/In2`**、`xlconcat_0/dout → processing_system7_0/IRQ_F2P`。所以断言的"route to IRQ_F2P[2]"**是真的**。 |
| L6–L7 | 我也核实过 BSP：`xparameters.h` 里**确实没有任何 `pd_ddr` 中断宏**。所以"不想猜中断号"是有据的，不是偷懒。 |
| L9–L12 | **前置条件写清楚**，这是好习惯。它告诉你：若这个程序失败，先怀疑这三条没满足，而不是先怀疑代码。 |

> **背景知识**：GIC（Generic Interrupt Controller）是 PS 里的中断控制器。中断号不是随便猜的，必须由 XSA 导出的硬件描述来决定。猜错中断号的后果是"程序能编过、能跑，但永远不进中断"——**静默失效**，最难查。

## 3.2 头文件（L14–L18）

```c
#include "xil_cache.h"     // Xil_DCacheInvalidateRange
#include "xil_io.h"        // Xil_In32 / Xil_Out32
#include "xil_printf.h"    // xil_printf
#include "xil_types.h"     // u32 / UINTPTR / UINTPTR 等
#include "xparameters.h"   // 由 XSA 自动生成的"硬件地址字典"
```

| 行 | 讲解 |
|---|---|
| L14 | `xil_cache.h` 提供 Cache 维护函数。**为什么裸机也要管 Cache**——见 §4.3，这是本程序最容易踩的坑。 |
| L15 | `xil_io.h` 提供 `Xil_In32(addr)` / `Xil_Out32(addr, val)`，本质是"一次 32 位内存映射读/写"。因为 PL 寄存器被映射到了 CPU 的地址空间，所以**读寄存器 = 读内存**。 |
| L16 | `xil_printf` 是 Xilinx 的精简 printf：不支持 `%f`（浮点）、缓冲区小、代码体积小。裸机里**优先用它**。 |
| 17 | `u32`、`UINTPTR` 这些是 Xilinx 的类型别名，比 `unsigned int` 更明确（尤其跨 32/64 位平台）。 |
| L18 | **`xparameters.h` 是理解一切的关键**。Vitis 在你导入 XSA 时自动生成它，里面是硬件里每个模块的基地址。你写代码时**永远不要手抄地址**，要引用这里的宏。 |

## 3.3 取基地址（L20–L26）

```c
#if defined(XPAR_PD_DDR_0_BASEADDR)
#define PD_DDR_BASE              ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
#elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#define PD_DDR_BASE              ((UINTPTR)XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#else
#error "Cannot find pd_ddr AXI-Lite base address in xparameters.h"
#endif
```

| 行 | 讲解 |
|---|---|
| L20 | `#if defined(...)` 是预处理条件编译：**编译期**判断这个宏存不存在。 |
| L21 | 首选宏。Vivado 里这个 cell 叫 `pd_ddr_0`，所以 Vitis 生成 `XPAR_PD_DDR_0_BASEADDR`。我实测其值 = **`0x40010000`**。 |
| L22–L23 | 备选宏。`pd_ddr_0` 内部实例化了 `pd_ddr_bd_adapter`，不同打包方式下 Vitis 可能用这个名字生成宏。**两级 fallback = 对工具版本变化的防御。** |
| L24–L25 | `#error` 是**编译期保护**。注意它比运行时检查更值钱：宁可不让你编过，也不要让你跑起来打错地址。**打错地址的后果是写坏别的外设，可能毫无征兆。** |
| L21 | `(UINTPTR)` 强制转换：算术运算要按指针宽度做，避免 32 位截断告警。 |

> **背景知识 · 本工程的地址地图**（我实测自 BD addressing 段）：
> ```
> pd_feature_0 = 0x4000_0000    pd_ddr_0  = 0x4001_0000
> pd_filter_0  = 0x4002_0000    axi_dma_0 = 0x4040_0000
> ```
> ⚠️ 注意：**你们这套和接口契约/同事那边是反的**（契约是 ddr 在前、feature 在后）。同事的驱动拿来跑你的 bit，会全部打错地址。这是已经记在案的上板隐患。

## 3.4 寄存器偏移表（L28–L45）

```c
#define DDR_CTRL                 0x000U
#define DDR_STATUS               0x004U
#define FREEZE_CTRL              0x034U
#define SLOT_CTRL                0x04CU
#define SLOT_STATUS              0x050U
#define SLOT_SEQ                 0x054U
#define SLOT_DROPS               0x058U
#define SNAP_TRIG_CTRL           0x05CU
#define SNAP_TRIG_DROPS          0x0A0U

#define SLOT_BASE_OFF(slot)      (0x060U + ((slot) * 0x10U))
#define SLOT_LEN_OFF(slot)       (0x064U + ((slot) * 0x10U))
#define SLOT_SEQ_OFF(slot)       (0x068U + ((slot) * 0x10U))
#define SLOT_FLAGS_OFF(slot)     (0x06CU + ((slot) * 0x10U))

#define DDR_SLOT0_BASE           0x20001000U
#define DDR_SLOT_SIZE            0x00C00000U
#define DDR_SLOT_AREA_END        0x23001000U
```

| 行 | 讲解 |
|---|---|
| L28–L36 | **这些全是"页内偏移"，不是绝对地址。** 真实地址 = `PD_DDR_BASE + 偏移`，例如 `0x40010000 + 0x050 = 0x40010050`。 |
| 尾部 `U` | C 语言里 `0x1` 是 `int`（有符号），`0x80000000U` 才是无符号。加 `U` 是为了避免移位/比较时的符号问题。**这条在跟硬件位掩码打交道时很重要。** |
| L30 | `FREEZE_CTRL = 0x034` 这个偏移是**接口契约规定的**（契约 §4.3 写 FREEZE_CTRL = 0x1034），实现里为了兼容把页内偏移固定成 0x34。 |
| L38–L41 | **函数式宏**：`SLOT_BASE_OFF(2)` 会展开成 `(0x060U + ((2) * 0x10U))` = `0x080`。4 个槽 × 每槽 4 个字（BASE/LEN/SEQ/FLAGS）× 每字 4 字节 = 每槽 0x10 字节，正好排满 `0x060`–`0x09F`。**所以 `SNAP_TRIG_DROPS` 从 `0x0A0` 开始**。 |
| L38 的括号 | 宏参数一定要加括号。写成 `(0x060U + slot * 0x10U)` 也还行，但一旦有人传 `SLOT_BASE_OFF(a+b)` 就会错。**函数式宏的括号习惯是硬功夫。** |
| L43–L45 | DDR 里四个槽的地理位置。我对着 `pd_ddr_defines.vh` 核过，**逐值一致**：`DDR_SLOT0_BASE 0x2000_1000`、`DDR_SLOT_SIZE 0x00C0_0000`(12 MiB)、`DDR_SLOT_AREA_END 0x2300_1000`（哨兵地址 = 末字节+1）。 |

> **背景知识 · 为什么槽基址是 `0x2000_1000` 而不是"看着更整齐"的 `0x2000_0000`**？
> 因为快照数据要被 DataMover（关 DRE）以 8 字节对齐写，且契约要求块 24 字节对齐，还要 4 KiB 页对齐。
> 三者求最小公倍数：`LCM(4096, 24) = 12288 = 0x3000`。而 `0x2000_0000` 缺因子 3，`mod 24 = 8` ——**一票否决**。
> 往前挪一格到 `0x2000_1000` 恰好补上：`0x1000 mod 24 = 16`，`16 + 8 = 24 ≡ 0`。
> 这类"看起来合规的陷阱"在别处也常见，值得记住这个推理过程。

## 3.5 位掩码定义（L47–L66）

```c
#define DDR_STATUS_COPY_BUSY     (1U << 1)
#define DDR_STATUS_COPY_DONE     (1U << 6)
#define DDR_STATUS_COPY_ERR      (1U << 8)
#define DDR_STATUS_CFG_ERR       (1U << 9)

#define SLOT_VALID_MASK          0x0000000FU
#define SLOT_BUSY_MASK           0x000000F0U
#define SLOT_LOCKED_MASK         0x00000F00U
#define SLOT_FULL                (1U << 12)
#define SLOT_CFG_ERR             (1U << 13)
#define SLOT_REQ_OVERFLOW        (1U << 14)
#define SLOT_CMD_ERR             (1U << 15)
#define SLOT_PENDING             (1U << 16)
#define SLOT_LAST_SHIFT          17U
#define SLOT_READY               (1U << 19)

#define TRIG_ENABLED             (1U << 0)
#define TRIG_MASK_ALL            (0xFU << 1)
#define TRIG_ARMED               (1U << 16)
#define TRIG_DROP                (1U << 17)
```

| 行 | 讲解 |
|---|---|
| L47–L50 | `DDR_STATUS`（0x04）的位。对照 RTL 读回拼接：`{22'd0, cfg_err, copy_err, copy_pending, copy_done_sticky, err, snap_overrun, freeze_done, hiwm, copy_busy, acq_en}` → bit0=acq_en、bit1=copy_busy、bit6=copy_done、bit8=copy_err、bit9=cfg_err。**逐位吻合。** |
| L48 | **注意它是 sticky（粘滞）**：一旦完成就一直是 1，直到你写触发/复位去清它。这是"事件型"状态的正确做法——否则 CPU 轮询时可能错过那一个时钟周期的脉冲。 |
| L52–L54 | 三组 4 位掩码（每通道/每槽 1 位）。`0x0000000F` 取低 4 位… `0x00000F00` 取 bit11–8。**用"掩码 + 移位"而不是逐个判断，是处理位域的标准手法。** |
| L55–L61 | `SLOT_STATUS`（0x50）的位。对照 RTL：`{12'd0, snapshot_ready, last[1:0], req_pending, cmd_err, req_overflow, cfg_err, full, locked[3:0], busy[3:0], valid[3:0]}`。<br>→ bit15=cmd_err、bit14=req_overflow、bit13=cfg_err、bit12=full、bit11:8=locked、bit7:4=busy、bit3:0=valid、bit16=pending、bit18:17=last、bit19=ready。**全对。** |
| L60 | `SLOT_LAST_SHIFT 17` 存的是**位移量**不是掩码——因为 `last` 是 2 位字段 `[18:17]`，要 `(status >> 17) & 0x3` 才能取出来（见 L204）。 |
| L63–L66 | `SNAP_TRIG_CTRL`（0x5C）的位。对照 RTL 读回：`{14'd0, slot_full_drop, armed, 11'd0, mask[3:0], en}` → bit0=en、bit4:1=mask、bit16=armed、bit17=drop。**全对。** |

> **务必区分**：`SLOT_SEQ`（0x54，自动快照序号）和 `SNAP_SEQ`（0x48，legacy 全局快照序号）是两个不同的计数器。本程序用 `SLOT_SEQ`。

## 3.6 两个"魔法数字"（L68–L69）

```c
#define SNAPSHOT_TIMEOUT         120000000U
#define POLL_PRINT_PERIOD        10000000U
```

| 行 | 讲解 |
|---|---|
| L68 | **这是"迭代次数"，不是秒。** 每次迭代做 2 次 AXI-Lite 读（每次都要经过 PS 互联到 PL，几十个时钟周期）。按 CPU 667 MHz、每次迭代约 70–100 周期粗算，1.2 亿次 ≈ **10 秒量级**。具体值以实测为准。 |
| L69 | 每 1000 万次打印一次 "WAIT"，即整个超时窗口内约打印 12 行。**作用：区分"程序在跑但没事件"与"程序已经卡死"**——这是裸机调试的救命习惯。 |

> **反面教材**：如果这里写成"按秒计时"，就需要读全局定时器（`XTime_GetTime()`），多一层依赖。用"迭代次数"实现简单，代价是**时间边界不精确**。做"有界等待"时这是可接受的权衡，但**要在注释里写明它不是精确时间**。

## 3.7 读写包装（L71–L79）

```c
static u32 reg_read(u32 off)
{
    return Xil_In32(PD_DDR_BASE + off);
}

static void reg_write(u32 off, u32 value)
{
    Xil_Out32(PD_DDR_BASE + off, value);
}
```

| 行 | 讲解 |
|---|---|
| L71 | `static` = **文件内可见**。裸机工程里全局符号越少越好（避免和 BSP 的库函数重名）。 |
| L73 | 把"基址 + 偏移"的算术封在一次调用里。**好处**：将来基址映射变了，只改 L26 一个地方。 |
| L76 | 同理。注意 `Xil_Out32` 会**自动设置 WSTRB = 0xF**（4 字节全写）——这一点在 §4.2 很关键。 |
| — | **为什么不加 `volatile`？** 因为 `Xil_In32`/`Xil_Out32` 内部已经用了 `volatile` 指针访问，编译器不会把它优化掉。自己再套一层 `volatile` 反而可能误导读者以为这里存在"编译器优化风险"。 |

> **背景知识 · 内存映射 I/O（MMIO）**：PL 的寄存器并没有"寄存器"这种物理实体给 CPU 用，而是被 AXI 总线映射成了**一段内存地址**。所以"读寄存器 = 从某个地址 load"，"写寄存器 = 往某个地址 store"，全部走普通 `ldr`/`str` 指令。这就是为什么 §4.3 的 Cache 问题会要命。

## 3.8 `verify_register_contract`（L81–L96）

```c
static void fail_stop(const char *reason);

static void verify_register_contract(void)
{
    u32 ring_base = reg_read(0x08U);
    u32 ring_size = reg_read(0x0CU);
    u32 snap_base = reg_read(0x20U);
    u32 snap_size = reg_read(0x24U);

    xil_printf("DDR_BASE=0x%08x RING=0x%08x/%u SNAP=0x%08x/%u\r\n",
               (u32)PD_DDR_BASE, ring_base, ring_size, snap_base, snap_size);
    if (ring_base != 0x10002000U || ring_size != 0x07FFE000U)
        fail_stop("unexpected ring address contract");
    if (snap_base != 0x18000000U || snap_size != 0x007FE000U)
        fail_stop("unexpected legacy snapshot contract");
}
```

| 行 | 讲解 |
|---|---|
| L81 | **前置声明**。因为 `fail_stop` 定义在下面（L98），而这里要调用它，C 语言要求先声明。**这是纯语法要求，不是设计选择。** |
| L85 | 读 `RING_BASE`（0x08）。这个寄存器在 RTL 里是**编译期固定、只读**的（直接返回宏 `DDR_RING_BASE`）。 |
| L86 | 读 `RING_SIZE`（0x0C）。 |
| L87–L88 | 读 `SNAP_BASE`/`SNAP_SIZE`（0x20/0x24）。注意这两个是**可读写**寄存器（RTL 复位值 = `DDR_SNAP_BASE`/`DDR_SNAP_SIZE`），所以这里读的是**上电默认值**。 |
| L90–L91 | 格式化打印。`%08x` = 8 位十六进制补零（适合看地址）；`%u` = 无符号十进制（适合看字节数，`0x07FFE000` = 134,209,536 一眼看不懂，十进制才直观）。<br>`(u32)PD_DDR_BASE` 强转是因为 `PD_DDR_BASE` 是 `UINTPTR`，而 `%x` 要 `unsigned int`——**变参函数的类型不匹配是 C 里的经典坑**。 |
| L92–L93 | **断言 1**：环形区必须是 `0x1000_2000` 起、`0x07FF_E000` 长。我对着 `pd_ddr_defines.vh` 核过：`DDR_RING_BASE = 32'h1000_2000`、`DDR_RING_SIZE = 32'h07FF_E000`，**一致**。终点 = `0x1800_0000`，正好接上快照区，128 MiB。 |
| L94–L95 | **断言 2**：legacy 快照区必须是 `0x1800_0000` 起、`0x007F_E000` 长。同上核对一致。 |

> **这一段的真正价值**：它是"**版本自检**"。假设你拿着旧 bit 配新软件（或反过来），地址分区一改，这里立刻就会 fail_stop 并打印真实值。**比"跑一半数据写错地方"好一万倍。**
> 这也是一个通用模式：**凡是"软件假设了硬件常量"的地方，上电都应该先读回来对一遍。**

## 3.9 `fail_stop`（L98–L109）

```c
static void fail_stop(const char *reason)
{
    xil_printf("SNAPSHOT_FAIL: %s\r\n", reason);
    xil_printf("DDR_STATUS=0x%08x SLOT_STATUS=0x%08x TRIG=0x%08x\r\n",
               reg_read(DDR_STATUS), reg_read(SLOT_STATUS),
               reg_read(SNAP_TRIG_CTRL));
    xil_printf("SLOT_DROPS=0x%08x TRIG_DROPS=0x%08x\r\n",
               reg_read(SLOT_DROPS), reg_read(SNAP_TRIG_DROPS));
    for (;;) {
        __asm__ volatile ("wfi");
    }
}
```

| 行 | 讲解 |
|---|---|
| L100 | 先打印**原因字符串**（人类可读），再打印**原始状态字**（机器可读）。这是排障的黄金组合。 |
| L101–L103 | 一次性把所有关键状态**全打出来**。设计意图很明显：**你不知道死在哪一步，所以把"案发现场"整体留档**。<br>如果只打印原因，事后你还要重跑一次才能看到状态；`fail_stop` 之后就死循环了，**没有第二次机会**。 |
| L104–L105 | `SLOT_DROPS`（自动请求被拒绝/溢出的累计数）和 `SNAP_TRIG_DROPS`（因四槽全满而被拒绝的自动触发数）。<br>**这两个计数是判断"是逻辑错了还是槽满了"的唯一线索**——省掉它能让你多查半天。 |
| L106–L108 | `for(;;)` 死循环，里面是 **`wfi` = Wait For Interrupt**。 |
| L107 | `__asm__ volatile` = 内联汇编。`volatile` 禁止编译器把它优化掉/挪位。 |

> **背景知识 · 为什么失败后要 `for(;;)` 而不是 `return`**
> `main()` 返回会回到启动代码 `crt0`，在裸机上通常表现为**重新初始化并再跑一遍 main**，或者跑到未定义地址。两种都很糟：你会看到日志刷屏，或者 CPU 跑飞后把某个外设写坏。
> **死循环 = 主动把 CPU 停在原地等你来看**。配合 `wfi`，CPU 还会降低功耗。

> **`wfi` 到底做了什么**：让 Cortex-A9 进入低功耗等待状态，直到有中断来唤醒。醒来后因为外层是 `for(;;)`，会立刻再次 `wfi`。所以效果是"**原地驻留**"。
> 注意：这不是"停机"——JTAG 调试器仍能 halt 它、读寄存器、单步。所以消息里才提醒"下载另一个 ELF 前要先 halt"。

## 3.10 `wait_snapshot_ready`（L111–L129）

```c
static u32 wait_snapshot_ready(void)
{
    u32 i;
    for (i = 0; i < SNAPSHOT_TIMEOUT; ++i) {
        u32 status = reg_read(SLOT_STATUS);
        u32 ddr_status = reg_read(DDR_STATUS);
        if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR))
            fail_stop("slot manager error");
        if (ddr_status & (DDR_STATUS_COPY_ERR | DDR_STATUS_CFG_ERR))
            fail_stop("DDR copy/config error");
        if (status & SLOT_READY)
            return status;
        if ((i % POLL_PRINT_PERIOD) == 0U)
            xil_printf("WAIT i=%u DDR_STATUS=0x%08x SLOT_STATUS=0x%08x\r\n",
                       i, ddr_status, status);
    }
    fail_stop("snapshot timeout");
    return 0U;
}
```

| 行 | 讲解 |
|---|---|
| L113 | 循环变量。**注意它是 `u32`**：`u32` 是 32 位无符号，最大 42.9 亿，1.2 亿不会溢出。如果这里写成 `u16`（最大 65535），循环**永远到不了** `SNAPSHOT_TIMEOUT`，就变成死循环了。 |
| L114 | 有界 for 循环。`i < SNAPSHOT_TIMEOUT` 保证一定退出。 |
| L115–L116 | 每次迭代读两个状态字。**顺序**：先读 `SLOT_STATUS` 再读 `DDR_STATUS`，两次读之间 PL 状态可能变——所以这两个值**不是同一时刻的快照**。做冒烟测试可以接受；如果要严格一致，应该读一个里面的"合并状态字"，或者接受这种轻微不一致并在注释里写明。 |
| L117–L118 | **先查错误**。三个槽管理器错误：`cfg_err`（配置错）、`req_overflow`（自动请求被拒绝累计溢出）、`cmd_err`（命令错）。<br>**为什么先查错再查 ready**：如果 ready 和 err 同时为 1，真实含义是"出过错、但恰好也有个槽好了"——这时候**必须先报错**，否则你会拿着可能不可信的数据继续跑。**判据的优先级顺序本身就是设计决策。** |
| L119–L120 | 再查 DDR 拷贝错误：`copy_err`（拷贝过程错）、`cfg_err`（快照配置错）。 |
| L121–L122 | 都干净，才检查 `SLOT_READY`。**这是唯一的成功出口**：返回 `status` 让调用者知道是哪一步成功的。 |
| L123–L125 | **心跳打印**。`i % POLL_PRINT_PERIOD == 0` 在 i=0 时也成立，所以第一条会立刻打印（等于"我开始等了"的信号）。 |
| L127 | 循环走完 = 超时。**超时的语义要说清楚**：它可能是"事件源根本没产生事件"，也可能是"PL 逻辑坏了"。这个函数的输出配上前面的 `WAIT_EVENT` 提示，就能让人区分。 |
| L128 | `return 0U;` —— 实际上**到不了**（`fail_stop` 是死循环）。加它是为了**消除编译器的"可能不返回"警告**。这类"防御性 return"在嵌入式里很常见。 |

> **与方案里写的"轮询 vs 中断"对比**：轮询的代价是 CPU 空转 ~10 秒；收益是**不依赖中断号、不依赖 GIC 配置、出错路径全部可见**。
> 对一次冒烟测试，这个交换是划算的。**但产品代码不能用这种做法**——将来上 IRQ 是必然的（计划书 §6.2 第 3 项）。

## 3.11 `verify_slot`（L131–L170）

```c
static void verify_slot(u32 slot, u32 status)
{
    u32 base = reg_read(SLOT_BASE_OFF(slot));
    u32 len = reg_read(SLOT_LEN_OFF(slot));
    u32 seq = reg_read(SLOT_SEQ_OFF(slot));
    u32 flags = reg_read(SLOT_FLAGS_OFF(slot));
    u32 *data;
    u32 first, last;

    xil_printf("SNAPSHOT slot=%u base=0x%08x len=%u seq=%u flags=0x%08x\r\n",
               slot, base, len, seq, flags);

    if ((status & SLOT_VALID_MASK) == 0U)
        fail_stop("ready asserted without valid slot");
    if ((base & 0x7U) != 0U || (len & 0x7U) != 0U || len == 0U)
        fail_stop("invalid slot alignment or length");
    if (base < DDR_SLOT0_BASE || base >= DDR_SLOT_AREA_END ||
        len > DDR_SLOT_SIZE || base + len > DDR_SLOT_AREA_END)
        fail_stop("slot range outside fixed DDR area");
    if ((status & SLOT_BUSY_MASK) != 0U)
        fail_stop("slot remains busy after snapshot_ready");

    /* Lock selected slot: SLOT_CTRL[7:4] is a W1P field, byte 0. */
    reg_write(SLOT_CTRL, 1U << (4U + slot));
    status = reg_read(SLOT_STATUS);
    if ((status & (1U << (8U + slot))) == 0U)
        fail_stop("slot lock did not stick");

    Xil_DCacheInvalidateRange((UINTPTR)base, len);
    data = (u32 *)(UINTPTR)base;
    first = data[0];
    last = data[(len / sizeof(u32)) - 1U];
    xil_printf("SNAPSHOT_DATA first=0x%08x last=0x%08x\r\n", first, last);

    /* Release selected slot: SLOT_CTRL[11:8] is W1P, byte 1. */
    reg_write(SLOT_CTRL, 1U << (8U + slot));
    status = reg_read(SLOT_STATUS);
    if ((status & (1U << (8U + slot))) != 0U)
        fail_stop("slot release did not clear lock");
}
```

| 行 | 讲解 |
|---|---|
| L133–L136 | 读该槽的 4 个字描述符。用 §3.4 的函数式宏算偏移，`slot` 是变量也能用。 |
| L137 | `u32 *data;` —— **指针**。一旦拿到 DDR 里的 `base` 地址，就可以像访问数组一样读它。这是"内存映射"的直接体现：**PL 写进 DDR 的字节，CPU 用普通指针就能读**。 |
| L140–L141 | 打印描述符。`seq` 是这一槽的快照序号，`flags` 是 `[2]=locked [1]=busy [0]=valid`。 |
| L143–L144 | **断言 1**：`SLOT_STATUS` 的 valid 位里必须至少有一个是 1，否则"ready 说好了，但没有哪个槽说自己是有效的"，自相矛盾。 |
| L145–L146 | **断言 2**：`base` 和 `len` 必须 8 字节对齐（低 3 位为 0），且 `len` 不为 0。<br>注意这里**只校验了 8 字节对齐**，而设计实际保证的是 **12288 字节（0x3000）整数倍**。做冒烟测试够用；要更严格可以写 `base % 12288 == 0`。 |
| L147–L149 | **断言 3**：地址必须在四槽区内（`[0x2000_1000, 0x2300_1000)`），且 `base + len` 不越过哨兵。<br>**这是防"越界读"的关键**：如果 PL 给的 base/len 有错，这里挡住，就不至于读到别的区域去。 |
| L150–L151 | **断言 4**：`snapshot_ready` 之后 `busy` 必须已经清零。逻辑上"准备好了却还在忙"是不合理的。 |
| L153 | 注释说明锁字段位置。 |
| L154 | **锁定槽**。写 `SLOT_CTRL = 1 << (4 + slot)`：slot=0 → `0x10`；slot=1 → `0x20`…… 这是"给第 slot 位置 1"。<br>**为什么要锁**：槽是"空闲 → 有效 → 已锁定 → 空闲"循环的。PS 正在读的时候，PL 不能把这个槽拿去装新数据，否则读到的半新半旧。**锁 = 借条。** |
| L155–L157 | 回读确认"锁真的锁上了"。**这是个极好的习惯**：W1P 脉冲写法如果位置算错、或 WSTRB 不对，写进去是**静默无效**的。写完之后回读验证，才能把"静默失效"变成"明确报错"。 |
| L159 | **`Xil_DCacheInvalidateRange(base, len)` —— 本程序最关键的一行。** 见 §4.3 详解。 |
| L160 | 把地址转成 `u32 *`。 |
| L161–L162 | 读**第一个**和**最后一个**字。`len / sizeof(u32)` = 字数，所以最后一个字的下标是 `字数 - 1`。<br>**这只是"能读通"的证明**，不是"数据正确"的证明。想证明数据正确，要跟 DDS 生成的已知波形比对。 |
| L165 | 注释说明释放字段位置。 |
| L166 | **释放槽**。写 `SLOT_CTRL = 1 << (8 + slot)`：slot=0 → `0x100`。 |
| L167–L169 | 回读确认锁已清。<br>**一进一出**：这个函数是"借了槽 → 读 → 还回去"的完整事务。**裸机代码里最忌讳只借不还**——那样 PL 会抱怨"槽不可用"，你下一次自动快照就失败了。 |

## 3.12 `main`（L172–L223）

```c
int main(void)
{
    u32 status, trig, seq_before, seq_after, slot;

    xil_printf("--- pd_snapshot_poll automatic snapshot test ---\r\n");
    verify_register_contract();

    /* Clear stale sticky status before enabling a new bounded run. */
    reg_write(SLOT_CTRL, (1U << 12));
    reg_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED);

    /* Enable ring acquisition and four-slot automatic copying. */
    reg_write(DDR_CTRL, 1U);
    reg_write(SLOT_CTRL, 1U);
    reg_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);

    trig = reg_read(SNAP_TRIG_CTRL);
    status = reg_read(SLOT_STATUS);
    seq_before = reg_read(SLOT_SEQ);
    xil_printf("ARM trig=0x%08x slot=0x%08x seq=%u\r\n",
               trig, status, seq_before);

    if ((trig & (TRIG_ENABLED | TRIG_MASK_ALL)) !=
        (TRIG_ENABLED | TRIG_MASK_ALL))
        fail_stop("automatic event trigger did not enable");
    if ((trig & TRIG_ARMED) == 0U)
        fail_stop("automatic event trigger is not armed");
    if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR))
        fail_stop("slot manager reported stale error");

    xil_printf("WAIT_EVENT: produce one DDS/feature event now\r\n");
    status = wait_snapshot_ready();
    slot = (status >> SLOT_LAST_SHIFT) & 0x3U;
    verify_slot(slot, status);

    seq_after = reg_read(SLOT_SEQ);
    if (seq_after <= seq_before)
        fail_stop("snapshot sequence did not advance");

    /* Resume is a W1P at FREEZE_CTRL[1]; this rearms event trigger logic. */
    reg_write(FREEZE_CTRL, 2U);
    trig = reg_read(SNAP_TRIG_CTRL);
    if ((trig & TRIG_ARMED) == 0U)
        fail_stop("freeze_resume did not rearm trigger");

    xil_printf("SNAPSHOT_POLL_PASS slot=%u seq=%u\r\n", slot, seq_after);
    xil_printf("STOP: CPU is parked; halt before downloading another ELF.\r\n");
    for (;;) {
        __asm__ volatile ("wfi");
    }
    return 0;
}
```

| 行 | 讲解 |
|---|---|
| L176 | 打一条横幅，便于在串口日志里定位本次运行。**裸机程序的第一行输出应该永远是"我是谁"。** |
| L177 | **第 1 步：契约自检**。地址分区不对就立刻停。**放在最前面**，因为后面所有操作都建立在"地址是对的"这个前提上。 |
| L179 | 注释：清掉上一次运行留下的粘滞状态。 |
| L180 | **清槽状态**。写 `SLOT_CTRL = 1<<12` = `0x1000`。<br>对照 RTL：`if (wstrb[1] && wdata[12]) o_slot_status_clear <= 1'b1;` → **bit12 属于 byte1**，而 `Xil_Out32` 会把 WSTRB 设成 `0xF`（4 字节全写），所以 `wstrb[1]` 为真，清状态脉冲发出。✅ |
| L181 | **清触发拒绝计数**。写 `SNAP_TRIG_CTRL = 0x100 | 0x1E | 0x1` = `0x11F`。<br>对照 RTL：`if (wstrb[0]) { en <= wdata[0]; mask <= wdata[4:1]; }` 和 `if (wstrb[1] && wdata[8]) stat_clear <= 1`。<br>`0x11F` 的 bit8=1 → 清计数；bit0=1 → 使能；bit4:1=`0xF` → 四通道全选。**一次写做完三件事。** |
| L183 | 注释：使能环采与四槽自动拷贝。 |
| L184 | **使能环采**。写 `DDR_CTRL = 1` → RTL 里 `o_acq_en <= wdata[0]` → 环形缓冲开始写 DDR。 |
| L185 | **使能四槽自动拷贝**。写 `SLOT_CTRL = 1` → 目标 bit0 = `auto_snap_en`。<br>RTL 写这里的条件很讲究（见 §4.2）：`if (wstrb[0] && (wdata[7:4]==0) && !(wstrb[1] && (|wdata[12:8]))) o_auto_snap_en <= wdata[0];`<br>本行 `wdata[7:4] = 0`、`wdata[12:8] = 0` → 条件成立 → 成功写入。✅ |
| L186 | **武装事件触发**。又写一次 `SNAP_TRIG_CTRL = 0x1F`（这次**不带** bit8）。<br>**为什么分两次写**：L181 带清计数，L186 不带——如果合成一次写，清计数和"武装"同时发生，语义不清。**"清脏状态"和"正式启动"分成两次写，是让因果链清晰。** |
| L188–L190 | 读回三个值。`seq_before` 是**这次运行前的快照序号**，用作后面"序号有没有递增"的基线。 |
| L191–L192 | 打印武装结果。**这一步的输出是排障时第一个该看的**：`trig=0x0001001f` 表示 en=1、mask=0xF、armed=1。 |
| L194–L196 | **断言：使能位和通道掩码都写对了**。注意写法 `(trig & (A|B)) != (A|B)` 的意思是"**A 和 B 必须都为 1**"（只查一位时用 `==0` 会更简单，查多位必须这样写）。 |
| L197–L198 | **断言：`armed` 必须是 1**。<br>对照 RTL：`o_armed = i_enable && !i_slot_full && !waiting_resume`。所以 armed=0 的含义可能是"没使能"、"四槽全满"、或"上一次冻结还没 resume"。**这个断言把三种可能一次性排除。** |
| L199–L200 | **断言：不能带着旧错误启动**。清完状态后再查一次，确保真的干净了。 |
| L202 | **提示人（或触发源）现在该产生事件了**。这条打印是给你看的——它在告诉你"程序接下来会安静地等，不是卡死了"。 |
| L203 | **进入等待**，直到 `SLOT_READY` 或出错或超时。 |
| L204 | 从 `status` 里取出 `last_slot`：`(status >> 17) & 0x3`。**2 位字段，掩码是 0x3。** |
| L205 | 校验并读出该槽（含"读→锁→cache 失效→取首末字→释放"）。 |
| L207–L209 | **断言：序号必须递增**。`seq_after > seq_before` 才算真的发生了一次快照。<br>这条断言很重要——它防止"`ready` 位是上一次遗留的、这次其实什么都没发生"这种假成功。 |
| L211 | 注释：`FREEZE_CTRL[1]` 是 resume（W1P），它会重新武装事件触发。 |
| L212 | **发 resume**。写 `FREEZE_CTRL = 2` = bit1。<br>对照 RTL：`if (wstrb[0] && wdata[1]) o_freeze_resume <= 1'b1;` ✅<br>**这一步不能省**：按设计，一次冻结/恢复只允许自动触发**一次**（防止连续事件把槽冲爆）。所以 PS 必须明确说"我处理完了，可以再来"。 |
| L213–L215 | **断言：resume 之后必须重新 armed**。这是对"重新武装"这条契约的直接验证。 |
| L217 | **成功的唯一标志**：`SNAPSHOT_POLL_PASS`。**注意它出现在最后**，前面每一步失败都会先 `fail_stop`，所以这条打印的含义非常强。 |
| L218 | 提醒：CPU 会驻留，再下载别的 ELF 前先 halt。 |
| L219–L221 | 驻留（同 §3.9）。**`main` 正常结束也一样要驻留**，不能让 CPU 掉回 `crt0`。 |
| L222 | 防御性 return，消除编译告警。 |

> **整体节奏总结**（背下这个模板，以后写任何"硬件自检"程序都能套）：
> ```
> ① 我是谁 → ② 前提自检（地址/版本） → ③ 清脏状态 → ④ 配置并确认生效
> → ⑤ 等待（有界 + 心跳） → ⑥ 处理结果 → ⑦ 断言结果自洽
> → ⑧ 复位到可重用状态 → ⑨ 打印唯一的成功标志 → ⑩ 驻留等人看
> ```

---

# §4 五个必须吃透的点

## 4.1 W1P —— 写 1 脉冲（Write-1-Pulse）

**是什么**：这类寄存器位"平时恒为 0"，你往它写 1，硬件内部产生**一个时钟周期的脉冲**去触发某个动作；你**永远读不到**它是 1。

本程序里用到 4 个 W1P：

| 寄存器 | 位 | 动作 |
|---|---|---|
| `SLOT_CTRL` | `[7:4]` | 锁定槽 lock |
| `SLOT_CTRL` | `[11:8]` | 释放槽 release |
| `SLOT_CTRL` | `[12]` | 清粘滞状态 |
| `SNAP_TRIG_CTRL` | `[8]` | 清触发拒绝计数 |
| `FREEZE_CTRL` | `[0]` / `[1]` | 冻结触发 / 恢复 |
| `DDR_CTRL` | `[1]` / `[2]` | 软复位 / 手动拷贝 |

**为什么这样设计**：动作类命令天然是"事件"不是"状态"。如果用普通可读写位，你写 1 之后必须记得写回 0，否则动作会反复触发——多一个状态就多一类 bug。

**踩坑点**：
- ❌ **不要试图读回这个位来确认它生效了**——它永远是 0。
- ✅ **要靠"副作用"确认**：例如锁槽之后读 `SLOT_STATUS[11:8]locked`；冻结之后等 `SLOT_STATUS[19]ready`。本程序 L155–L157 / L167–L169 就是这么做的。
- ⚠️ **注意 WSTRB 字节位置**：`SLOT_CTRL` 的 lock 在 bit7:4（byte0），release 在 bit11:8（byte1）。你用 `Xil_Out32` 时 WSTRB 自动全 1，没问题；但如果将来用 `Xil_Out16` 或者手动配 WSTRB，**写错字节这个命令就静默无效**。

## 4.2 为什么写 `SLOT_CTRL` 那么绕 —— RTL 里那个"聪明"的保护条件

RTL 原文（`pd_ddr_axil.v` L277–L282）：

```verilog
A_SLOT_CTRL: begin
    if (wstrb[0] && (wdata[7:4] == 4'b0) &&
        !(wstrb[1] && (|wdata[12:8])))
        o_auto_snap_en <= wdata[0];
    if (wstrb[0]) o_slot_lock    <= wdata[7:4];
    if (wstrb[1]) o_slot_release <= wdata[11:8];
    if (wstrb[1] && wdata[12]) o_slot_status_clear <= 1'b1;
end
```

**问题背景**：`auto_snap_en` 是 bit0，而 lock 是 bit7:4、release 是 bit11:8、clear 是 bit12。如果写 `SLOT_CTRL = 0x10`（只想锁 slot0），那么 bit0 恰好是 0 —— 如果 RTL 简单地写 `o_auto_snap_en <= wdata[0]`，**就会顺手把自动快照关掉**。这是个"看起来对、实际很坑"的设计。

**RTL 的解法**：只有当"没有同时写命令字段"时，才更新 `auto_snap_en`：
- `wdata[7:4] == 0` → 本次没写 lock
- `!(wstrb[1] && |wdata[12:8])` → 本次没写 release / clear

**在这个程序里的实际行为验证**（这是最能体现"软件和硬件必须一起看"的地方）：

| 程序里的写 | 值 | lock 写入? | release/clear 写入? | `auto_snap_en` 会变吗 |
|---|---|---|---|---|
| L185 `SLOT_CTRL = 1` | `0x0001` | 否（[7:4]=0） | 否（[12:8]=0） | ✅ **会被设为 1**（本来就想开） |
| L154 `1U << (4+slot)`，slot=0 | `0x0010` | 是（=1） | 否 | ✅ 条件不满足 → **保持不变** |
| L166 `1U << (8+slot)`，slot=0 | `0x0100` | 否 | 是（release=1） | ✅ 条件不满足 → **保持不变** |
| L180 `1U << 12` | `0x1000` | 否 | 是（clear=1） | ✅ 条件不满足 → **保持不变** |

**所以这套写序是"设计过"的，不是碰运气。** 换一个顺序（比如把 `SLOT_CTRL=1` 写在锁槽之后）就可能把 auto_snap_en 关掉，表现为"程序没报错但再也不产生快照"——**典型的静默失效**。

## 4.3 Cache 一致性 —— 本程序最关键的一行

### 4.3.1 硬件事实（有原文依据）

`.../bsp/libsrc/standalone/src/arm/cortexa9/gcc/translation_table.S` 第 23–29 行：

```
*| DDR                   | 0x00000000 - 0x3FFFFFFF | Normal write-back Cacheable  |
*| PL                    | 0x40000000 - 0xBFFFFFFF | Strongly Ordered             |
*| Memory mapped devices | 0xE0000000 - 0xE02FFFFF | Device Memory                |
*| SRAM                  | 0xE4000000 - 0xE5FFFFFF | Normal write-back Cacheable  |
```

**两个结论直接从这里读出来：**

1. **DDR（`0x0000_0000`–`0x3FFF_FFFF`）是可缓存的（write-back）** → 快照数据放在 DDR，PL 通过 HP 口直接写进去，**CPU 完全不知情**。如果 CPU 之前正好缓存了那片地址，它读到的会是**旧的缓存副本**。
2. **PL 区（`0x4000_0000`–`0xBFFF_FFFF`）是 Strongly Ordered** → AXI-Lite 寄存器访问**本来就不走 Cache**。所以 `Xil_In32`/`Xil_Out32` 读寄存器**不需要任何 cache 维护**。

### 4.3.2 所以为什么必须有 `Xil_DCacheInvalidateRange`

`Xil_DCacheInvalidateRange((UINTPTR)base, len)` 做的是：**把 CPU 侧对应这段 DDR 的缓存行作废（丢弃）**。作废之后，下一次读这段地址就会**真的去 DDR 取**，从而拿到 PL 刚写进去的新数据。

**"Invalidate" vs "Flush" 的区别（别搞反）**：
| 操作 | 做什么 | 什么时候用 |
|---|---|---|
| **Invalidate**（作废） | 丢掉缓存副本，下次读从内存取 | **CPU 要读"别人写的"数据** ← 本程序 |
| **Flush**（回写） | 把缓存里改过的数据写回内存 | CPU 写了数据，要让"别人"（如 DMA）看到 |

这个方向性错误是嵌入式经典 bug：该 invalidate 时 flush 了 → 读到旧数据；该 flush 时 invalidate 了 → **丢数据**。

### 4.3.3 常见疑问

**Q：`len` 是不是任意长度都行？**
A：不是。Cache 是按"缓存行"（Cortex-A9 通常是 32 字节）操作的。传进来的 `len` 如果不是行长的整数倍，函数内部会**按行对齐向上取整**处理。这可能作废掉你 range 之外的相邻数据——那份数据如果是脏的就会被丢掉。**安全做法：让快照的基址和长度都对齐到 32 字节（本设计 12288 字节对齐，绰绰有余）。**

**Q：REF 里说"快照区 MMU 设成 non-cacheable"，那这行还有必要吗？**
A：两个层面：① 当前这个 BSP 用的是**默认页表**，DDR 是可缓存的，所以这行**必需**；② 如果将来真把快照区改成 non-cacheable 映射，这行会变成**无害的空操作**（保留它更安全）。

**Q：读寄存器那边要不要也加一行？**
A：**不要**。PL 区是 Strongly Ordered，本来就不经过 Cache。加了不影响正确性，但会让人误以为那里有 cache 问题，反而掩盖真实原因。

## 4.4 为什么用轮询，不用中断

| 维度 | 轮询（本程序） | 中断 |
|---|---|---|
| 依赖 | 只要有 AXI 通路就行 | 需要 GIC 配置 + **正确的中断号宏** |
| CPU 开销 | 空转 ~10 秒 | 几乎为零 |
| 出错路径 | 每一步都能立刻打印 | 中断上下文里打印受限制 |
| 漏事件风险 | 快照是"状态位"不是"脉冲"，**不会漏** | 同样不漏 |
| 适合场景 | **一次性冒烟测试** | 产品运行 |

**关键理由**：`SLOT_STATUS[19]` 是**状态位**（ready 会一直保持，直到被清），不是单周期脉冲。所以轮询**不会漏掉事件**——这是轮询在这里可行的前提。如果它是个脉冲位，轮询就可能错过，那时必须用中断。

**升级到中断时要注意什么**（给将来做的准备）：
1. 需要一个**命名的中断号宏**。现在 BSP 里没有（我核过 `xparameters.h`，零命中），需要先在 BD/Vitis 里把它暴露出来，**不要手工猜数字**。
2. 中断服务函数里**不能长时间阻塞**（不能在里面 `xil_printf` 大段文本、不能等 AXI 事务）。
3. 典型的做法：ISR 里只置一个 `volatile` 标志 + 记录状态字，主循环里再做实际处理。**"ISR 短、主循环长"是嵌入式铁律。**
4. 别忘了处理**粘滞状态**：ISR 里应清掉触发源，否则会反复进中断。

## 4.5 `wfi` 与"驻留"

**`wfi` = Wait For Interrupt**。让 Cortex-A9 暂停执行直到有中断。本程序用 `for(;;) { wfi; }` 实现"**把 CPU 停在原地**"。

**为什么不直接 `while(1);`**：
- 纯空转会让 CPU 全速耗电发热；
- `wfi` 让核心进入低功耗状态，**但不影响调试器**（JTAG 仍可 halt、读寄存器、单步）。

**为什么必须驻留而不是 `return`**：`main` 返回会掉回 `crt0` 启动代码，通常表现为**重新初始化 + 再跑一次 main**，于是日志刷屏、状态被重置，你什么都看不到。**失败和成功都要驻留，这是裸机调试器友好的做法。**

**注意一个细节**：`wfi` 会被**任何中断**唤醒（即使该中断没被使能路由到 CPU）。所以严格说它不是"停机"，而是"睡着了会被吵醒、醒来立刻又睡"。对"驻留等人看"这个目的完全够用。

---

# §5 自测题（先自己答，再回头看 §3）

1. **为什么 `SNAPSHOT_TIMEOUT` 用"迭代次数"而不用"秒"？这样做的代价是什么？**
   > 提示：§3.6。代价是时间边界不精确、依赖 CPU 频率。

2. **`SLOT_VALID_MASK` 是 `0x0000000F`，`SLOT_LOCKED_MASK` 是 `0x00000F00`。如果有人把这两个写反了，程序会怎么表现？**
   > 提示：一个查"有效"、一个查"已锁"。写反了不会编译错，但断言会误判——**这类 bug 只能靠读懂位域来发现**。

3. **在 `verify_slot` 里，为什么必须先 lock 再读数据，读完还要 release？只 lock 不 release 会怎样？**
   > 提示：§4.1 + "槽是循环使用的"。不 release → 锁位一直为 1 → 该槽永远不可用 → 四次之后自动快照全部被拒绝（`SNAP_TRIG_DROPS` 涨）。

4. **如果把 L159 的 `Xil_DCacheInvalidateRange` 删掉，程序会怎么表现？**
   > 提示：§4.3。DDR 是可缓存的。表现可能是"首末字读出来是对的、但中间大面积是旧数据"，也可能完全正常——**取决于之前 CPU 有没有访问过那片内存**。这种"有时对有时错"的 bug 最难查，所以这一行不能省。

5. **`o_armed = i_enable && !i_slot_full && !waiting_resume`。程序在 L197 检查 `armed`，在 L214 再次检查。两次检查分别排除了哪些失败可能？**
   > 提示：第一次排除"没使能/槽满/残留未 resume"；第二次专门验证 "resume 真的起作用了"。

6. **如果串口上你看到的是 `WAIT_EVENT: ...` 之后没有任何输出，一直到最后打印 `SNAPSHOT_FAIL: snapshot timeout`，说明什么？下一步该查哪里？**
   > 提示：先区分"事件源没产生事件"和"PL 逻辑坏了"。查 `SNAP_TRIG_DROPS`（是否四槽全满）、查 `pd_feature_0` 是否真的产生了 `o_event_accept`。

7. **为什么 `fail_stop` 里要一口气打印 5 个寄存器的值，而不是只打印失败原因？**
   > 提示：§3.9。它之后就不返回了，**没有第二次读的机会**。

---

# 附：本程序访问的寄存器速查表

页基址 `PD_DDR_BASE = 0x4001_0000`（由 `XPAR_PD_DDR_0_BASEADDR` 得到，我实测确认）

| 偏移 | 名称 | 读写 | 位定义 | 本程序在哪用 |
|---:|---|---|---|---|
| `0x00` | `DDR_CTRL` | RW | `[0]`acq_en `[1]`sw_rst(W1P) `[2]`snap_start(W1P) | L184 |
| `0x04` | `DDR_STATUS` | RO | `[0]`acq_en `[1]`copy_busy `[2]`hiwm `[3]`freeze_done `[4]`snap_overrun `[5]`err `[6]`copy_done(**sticky**) `[7]`copy_pending `[8]`copy_err `[9]`snapshot_cfg_err | L116,L119 |
| `0x08` | `RING_BASE` | RO | 编译期固定 | L85 |
| `0x0C` | `RING_SIZE` | RO | 编译期固定 | L86 |
| `0x10` | `RING_WR_PTR` | RO | 环内字节偏移 | — |
| `0x20` | `SNAP_BASE` | RW | 复位默认 = `0x1800_0000` | L87 |
| `0x24` | `SNAP_SIZE` | RW | 复位默认 = `0x007F_E000` | L88 |
| `0x34` | `FREEZE_CTRL` | RW/RO | `[0]`trig(W1P) `[1]`resume(W1P) / `[8]`done | L212 |
| `0x4C` | `SLOT_CTRL` | RW | `[0]`auto_snap_en `[7:4]`lock(W1P) `[11:8]`release(W1P) `[12]`status_clear(W1P) | L154,L166,L180,L185 |
| `0x50` | `SLOT_STATUS` | RO | `[3:0]`valid `[7:4]`busy `[11:8]`locked `[12]`full `[13]`cfg_err `[14]`req_overflow `[15]`cmd_err `[16]`pending `[18:17]`last_slot `[19]`snapshot_ready | L115,L155,L167 |
| `0x54` | `SLOT_SEQ` | RO | 自动快照成功序号 | L190,L207 |
| `0x58` | `SLOT_DROPS` | RO | 自动请求被拒/溢出累计 | L104 |
| `0x5C` | `SNAP_TRIG_CTRL` | RW/RO | `[0]`event_trig_en `[4:1]`event_ch_mask `[8]`drop_count_clear(W1P) / `[16]`armed `[17]`drop | L181,L186,L188,L213 |
| `0x60/0x70/0x80/0x90` | 槽 0..3 `BASE` | RO | 编译期固定（`0x2000_1000` + n×`0x00C0_0000`） | L133 |
| `0x64/0x74/0x84/0x94` | 槽 0..3 `LEN` | RO | 本次拷贝长度 | L134 |
| `0x68/0x78/0x88/0x98` | 槽 0..3 `SEQ` | RO | 该槽快照序号 | L135 |
| `0x6C/0x7C/0x8C/0x9C` | 槽 0..3 `FLAGS` | RO | `[0]`valid `[1]`busy `[2]`locked | L136 |
| `0xA0` | `SNAP_TRIG_DROPS` | RO | 因四槽全满被拒的自动触发数 | L105 |

**DDR 内数据布局**（`pd_ddr_defines.vh` + `pd_ddr_axil.v` 实测核对一致）

| 区域 | 范围 | 大小 |
|---|---|---|
| 环形采集区 | `0x1000_2000` – `0x1800_0000` | 128 MiB − 8 KiB |
| legacy 快照区（手动调试） | `0x1800_0000` – `0x187F_E000` | 8 MiB − 8 KiB |
| 四槽区（自动快照） | `0x2000_1000` – `0x2300_1000` | 48 MiB（4 × 12 MiB） |

---

*本讲解中所有寄存器位定义均取自 `pd_ddr_axil.v`（L134–L174 地址常量、L253–L294 写译码、L314–L371 读多路）、
所有 DDR 常量均取自 `pd_ddr_defines.vh`，Cache 属性取自 BSP 的 `cortexa9/gcc/translation_table.S`，
中断连线取自 `pd_feature_bd.bd` 顶层 nets 段。未标注出处的判断均为笔者推论，已在文中说明。*
