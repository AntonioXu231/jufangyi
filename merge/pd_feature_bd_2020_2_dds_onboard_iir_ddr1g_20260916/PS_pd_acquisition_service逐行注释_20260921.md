# `pd_acquisition_service.c` · 逐行注释

> 面向：PS 侧裸机 C（Vitis 2024.1 standalone）。
> 定位：这是 **A+B 两档案的长跑采集服务 V1** —— 把「冒烟测试」升级成「带 UART 命令行的常驻服务」。
> 本文除了逐行解释，**还把偏移/位域/内存布局逐条对照了 RTL**，结论见 §13。

---

## 0. 它比冒烟测试多了什么

| 能力 | 冒烟测试 | 本服务 |
|---|---|---|
| 运行方式 | 跑完就 `wfi` | **常驻**，UART 命令控制 |
| 事件流（AXIS→DMA） | 一次性收 128 包 | 持续收，**写入 16 槽事件归档环** |
| 快照（四槽） | 验一次描述符 | **搬进 4 槽快照归档环**，并释放硬件槽 |
| 元数据 | 打印 | **`g_acq` 结构体**，供未来上位机协议读取 |
| 停止 | 无 | `STOP` → **干净停机**（排空在途快照后退出） |
| 覆盖策略 | 无 | 环形覆盖，`*_overwrites` **显式计数** |

**两条职责线（程序开头就写明）：**
```
AXIS 特征包  → RX 缓冲(0x01100000) → 事件归档环(16 × 64 KiB)
原始 DDR 快照 → 锁槽 → 拷贝 → 快照归档环(4 × 12 MiB) → 解锁槽
```
> 设计要点：**归档环只是"暂存"，不是协议**。将来的以太网/UART 上位机协议再来读 `g_acq` 和这两块区间，**不需要改 PL 的采集契约**。

---

## 1. 头文件

```c
#include "xparameters.h"    // 外设基地址宏
#include "xaxidma.h"        // AXI DMA 驱动
#include "xil_cache.h"      // Flush / Invalidate
#include "xil_io.h"         // Xil_In32 / Xil_Out32
#include "xil_printf.h"     // 串口打印
#include "xil_types.h"      // u32/s16…
#include "xstatus.h"        // XST_SUCCESS / XST_FAILURE
#include "xuartps_hw.h"     // XUartPs_IsReceiveData / XUartPs_RecvByte（寄存器级，不用中断）
#include <string.h>         // strcmp / strncmp / memset / memcpy
```
- **注意引入了 `xuartps_hw.h` 而不是 `xuartps.h`**：前者是**寄存器级**接口，够用且轻量；后者是完整驱动（带中断、环形缓冲），本服务刻意不用中断。

---

## 2. 三个基地址的条件编译（与冒烟测试同构）

```c
#if defined(XPAR_PD_DDR_0_BASEADDR)
# define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
#elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
...
```
- `pd_ddr_0` 的 AXI-Lite 基址（实测 `0x4001_0000`）。两种宏名对应两代 BSP 风格。

```c
#if defined(XPAR_XAXIDMA_0_BASEADDR)
# define DMA_BASE       ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG XPAR_XAXIDMA_0_BASEADDR     // SDT 流程：用基地址查找
#elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
# define DMA_BASE       ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG XPAR_AXIDMA_0_DEVICE_ID     // 经典 BSP：用器件号查找
```
- 两种 BSP 下 `XAxiDma_LookupConfig()` 的参数含义不同，所以分开定义。

```c
#if defined(XPAR_PS7_DDR_0_BASEADDRESS) ... # define PS_DDR_BASE ...
```
- PS DDR 基址（`0x0010_0000`）。

---

## 3. `pd_ddr` 寄存器映射

```c
#define DDR_CTRL          0x000U   // [0]acq_en [1]sw_rst(W1P) [2]snap_start(W1P)
#define DDR_STATUS        0x004U   // [0]acq_en [1]copy_busy … [5]err [8]copy_err [9]cfg_err
#define FREEZE_CTRL       0x034U   // [0]trig(W1P) [1]resume(W1P)
#define SLOT_CTRL         0x04CU   // [0]auto_snap_en [7:4]lock(W1P) [11:8]release(W1P) [12]status_clear(W1P)
#define SLOT_STATUS       0x050U
#define SLOT_SEQ          0x054U   // 自动快照成功序号（单调递增，status_clear 不清）
#define SLOT_DROPS        0x058U
#define SNAP_TRIG_CTRL    0x05CU   // [0]en [4:1]mask [8]drop_count_clear(W1P) / RO[16]armed
#define SLOT_BASE_OFF(n)  (0x060U + ((n) * 0x10U))   // 四槽描述符步长 0x10
#define SLOT_LEN_OFF(n)   (0x064U + ((n) * 0x10U))
#define SLOT_SEQ_OFF(n)   (0x068U + ((n) * 0x10U))
#define SLOT_FLAGS_OFF(n) (0x06CU + ((n) * 0x10U))
```
- 与 `pd_ddr_axil.v:134-174` 的 `A_*` localparam **逐一核对一致**（见 §13）。

```c
#define DDR_COPY_BUSY      (1U << 1)
#define DDR_RING_ERR       (1U << 5)    // 四路 OR，含 ring_ovf（启动瞬态那个）
#define DDR_COPY_ERR       (1U << 8)
#define DDR_CFG_ERR        (1U << 9)
#define SLOT_VALID_MASK    0x0000000FU  // [3:0]  valid
#define SLOT_BUSY_MASK     0x000000F0U  // [7:4]  busy
#define SLOT_LOCKED_MASK   0x00000F00U  // [11:8] locked   ← 本文件未使用
#define SLOT_CFG_ERR       (1U << 13)
#define SLOT_REQ_OVERFLOW  (1U << 14)
#define SLOT_CMD_ERR       (1U << 15)
#define SLOT_LAST_SHIFT    17U          // [18:17] last_slot
#define SLOT_READY         (1U << 19)   // 电平保持，直到 status_clear
```
- 与 `pd_ddr_axil.v:341-345` 一致（见 §13）。

```c
#define TRIG_ENABLED  (1U << 0)
#define TRIG_MASK_ALL (0xFU << 1)   // [4:1] 四通道全开
#define TRIG_ARMED    (1U << 16)    // 只读回位
```

### AXI DMA S2MM 寄存器（PG021）

```c
#define S2MM_DMACR_OFFSET   0x30U
#define S2MM_DMASR_OFFSET   0x34U
#define S2MM_LENGTH_OFFSET  0x58U
#define DMASR_HALTED        0x00000001U
#define DMASR_IDLE          0x00000002U
#define DMASR_ERROR_MASK    0x00004070U   // = bit14|bit6|bit5|bit4
```
- `0x4070 = 0100_0000_0111_0000` → bit4 `DMAIntErr`、bit5 `DMASlvErr`、bit6 `DMADecErr`、bit14 `Err_Irq`。**一次覆盖四类硬错误**，与 PG021 一致。

---

## 4. DDR 内存布局（本文件新增的关键内容）

```c
#define RX_BUFFER_BASE      ((UINTPTR)PS_DDR_BASE + 0x01000000U)   // = 0x0110_0000
#define RX_BUFFER_BYTES     65528U
#define EVENT_ARCHIVE_BASE  0x27000000U
#define EVENT_ARCHIVE_COUNT 16U
#define EVENT_ARCHIVE_STRIDE 0x00010000U   // 64 KiB
#define SNAP_ARCHIVE_BASE   0x24000000U
#define SNAP_ARCHIVE_COUNT  4U
#define SNAP_ARCHIVE_STRIDE 0x00C00000U    // 12 MiB
#define SNAP_SLOT_LOW       0x20001000U    // 硬件四槽区下界
#define SNAP_SLOT_HIGH      0x23001000U    // 硬件四槽区哨兵上界
```

**实测核算（已脚本验证）**：

| 段 | 起 | 止（独占） | 大小 |
|---|---|---|---|
| RX 接收缓冲 | 0x0110_0000 | 0x0110_FFF8 | 65,528 B |
| legacy SNAP（v1.6.0 手动路径） | 0x1800_0000 | 0x187F_E000 | 7.99 MiB |
| **硬件四槽区** | 0x2000_1000 | 0x2300_1000 | 48 MiB |
| **SNAP 归档环** 4 × 12 MiB | 0x2400_0000 | 0x2700_0000 | 48 MiB |
| **EVENT 归档环** 16 × 64 KiB | 0x2700_0000 | 0x2710_0000 | 1 MiB |
| PL 原始环形区（另一文档） | 0x1000_2000 | 0x1800_0000 | 128 MiB − 8 KiB |

- **无重叠**，最高用到 0x2710_0000 = 625 MiB < 1 GiB ✅
- ⚠️ **余量提醒**：单包上限 `RX_BUFFER_BYTES = 65,528`，而事件归档步长 `0x10000 = 65,536` —— **只差 8 字节**。当前安全（`bytes > RX_BUFFER_BYTES` 会 `halt`），但建议把 stride 提到 96 KiB，或在写归档前显式断言 `bytes <= EVENT_ARCHIVE_STRIDE`。

> 注释里说"these ranges are after the PL raw ring"——**措辞与事实相反**：RX 缓冲在 `0x0110_0000`，是在环形区（`0x1000_2000`）**之下**。不影响正确性，但读注释会误导。

---

## 5. 数据结构

```c
typedef struct {
    u32 sequence;      // 事件序号（全局单调）
    u32 ddr_addr;      // 在归档环里的地址
    u32 bytes;         // 本包字节数
    u32 peak_words;    // 本包里 type=0x00 的字数
    u32 cycle_words;   // 本包里 type=0x01 的字数
} pd_event_record_t;
```
- 每个归档项一条元数据。**注意 `ddr_addr` 是"归档后的地址"，不是原始的**——上位机拿它去读归档环，不是读 RX 缓冲。

```c
typedef struct {
    u32 sequence;          // 归档序号
    u32 source_slot;       // 来自哪个硬件槽（0..3）
    u32 source_addr;       // 硬件槽基址
    u32 archive_addr;      // 归档地址
    u32 bytes;
    u32 hw_slot_sequence;  // 硬件侧 SLOT_SEQ（用于和 PL 对账）
    u32 flags;             // 槽 FLAGS 快照
} pd_snapshot_record_t;
```
- `hw_slot_sequence` 是**跨世界的对账锚点**：PS 归档序号与 PL 的 `SLOT_SEQ` 一一对应，可以查出"有没有漏搬"。

```c
typedef struct {
    u32 magic;              // "PDAQ"
    u32 version;
    u32 event_sequence;     // 已归档事件总数
    u32 snapshot_sequence;
    u32 event_overwrites;   // 环形覆盖次数（显式，不藏）
    u32 snapshot_overwrites;
    u32 dma_errors;
    u32 slot_errors;
    u32 last_dma_status;    // 出错现场三件套
    u32 last_ddr_status;
    u32 last_slot_status;
    pd_event_record_t    event[16];
    pd_snapshot_record_t snapshot[4];
} pd_acq_shared_t;
```
- **把它设计成"共享结构体"是很有远见的一步**：将来上位机协议只要拿到这个结构体的地址（或内容），就能知道"收到了什么、在哪、有没有丢"。**无需改 PL 契约**。

```c
volatile pd_acq_shared_t g_acq;   // 故意做成全局：上位机协议可直接取地址
static XAxiDma g_dma;
static volatile u32 g_running;        // 1=正在采集
static volatile u32 g_stop_request;   // UART 置位，主循环轮询
static volatile u32 g_quit_request;
static volatile u32 g_start_request;
static volatile u32 g_start_limit;    // 0 = 连续跑
static char g_uart_line[48];
static u32  g_uart_len;
```
- **`volatile` 是必须的**：这些变量会被 `uart_poll()`（在 DMA 等待循环里）改写，而主循环读它们。没有 `volatile`，编译器可能把它们提到循环外，导致"敲了 STOP 不生效"。

---

## 6. 工具函数

```c
static u32 ddr_read(u32 off)  { return Xil_In32(PD_DDR_BASE + off); }
static void ddr_write(u32 off, u32 v) { Xil_Out32(PD_DDR_BASE + off, v); }
static u32 dma_read(u32 off)  { return Xil_In32(DMA_BASE + off); }
```
- 三个"页基址 + 偏移"的薄封装。`Xil_In32/Out32` 内部带内存屏障与 `volatile` 语义，保证真的访问寄存器。

```c
static void print_status(void) {
    xil_printf("STATUS run=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u ", ...);
    xil_printf("ddr=%08x slot=%08x dma=%08x drops=%u\r\n",
               ddr_read(DDR_STATUS), ddr_read(SLOT_STATUS),
               dma_read(S2MM_DMASR_OFFSET), ddr_read(SLOT_DROPS));
}
```
- `STATUS` 命令的输出：软件侧计数 + 硬件侧寄存器**并排打**，便于一眼对照（软件说 N 个，硬件说几个）。

```c
static u32 parse_u32(const char *p, u32 default_value) {
    u32 value = 0U, any = 0U;
    while (*p == ' ') ++p;                         // 跳空格
    while (*p >= '0' && *p <= '9') { any = 1U; value = value*10U + (u32)(*p - '0'); ++p; }
    return any ? value : default_value;            // 没数字就用默认值
}
```
- 极简十进制解析。**没有溢出保护**（输入 20 位数字会绕回）——对调试命令可接受，但若将来接入正式协议要补。

---

## 7. 打印归档记录

```c
static void print_event_record(u32 index) {
    if (index >= EVENT_ARCHIVE_COUNT || g_acq.event[index].sequence >= g_acq.event_sequence) {
        xil_printf("ERR EVENT index has no valid record\r\n"); return;
    }
    xil_printf("EVENT index=%u seq=%u addr=%08x bytes=%u peaks=%u cycles=%u\r\n", ...);
}
```
- **有效性判据**：`record.sequence < event_sequence`。因为 `event_sequence` 是"已归档总数"，而每条记录的 `sequence` 是它自己的序号；环里最新那条的 `sequence == event_sequence-1`，所以 `< event_sequence` 恰好筛掉"还没写过的槽"。
- 环覆盖后依然成立（例：`event_sequence=20`，index 3 里是 seq 19，`19 < 20` ✅）。

```c
static void print_snapshot_record(u32 index) { ... 同样结构 ... }
```

```c
static void clear_metadata(void) {
    if (g_running) { xil_printf("ERR CLEAR is accepted only while idle\r\n"); return; }
    memset((void *)&g_acq, 0, sizeof(g_acq));
    g_acq.magic = 0x50444151U;   // "PDAQ"
    g_acq.version = 1U;
    xil_printf("OK CLEAR metadata\r\n");
}
```
- **只清元数据，不清归档数据区**。且**必须在空闲时**才能清 —— 否则会把正在写的记录抹掉。

---

## 8. 命令解析 `execute_command`

```c
if (strcmp(line, "HELP") == 0) { xil_printf("CMD: START [packets] | STOP | STATUS | EVENT n | SNAP n | CLEAR | QUIT\r\n"); }
```
```c
else if (strncmp(line, "START", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
    if (g_running) xil_printf("ERR already running\r\n");
    else { g_start_limit = parse_u32(line + 5, PD_ACQ_PACKET_LIMIT); g_start_request = 1U; }
}
```
- **边界检查很到位**：不仅比对前 5 个字符，还要求第 6 个字符是 `'\0'` 或空格 —— 否则 `STARTER` 这种会被误认。
- `START`（不带数字）→ 默认 `PD_ACQ_PACKET_LIMIT=128`；`START 0` → 连续跑到 `STOP`。
- **命令解析只置标志位，不直接干活**：真正的动作在主循环里做（避免在解析过程中重入）。

```c
else if (strcmp(line, "STOP") == 0) {
    if (!g_running) xil_printf("OK already idle\r\n");
    else { g_stop_request = 1U; xil_printf("OK stop requested\r\n"); }
}
else if (strcmp(line, "STATUS") == 0) print_status();
else if (strncmp(line, "EVENT ", 6) == 0) print_event_record(parse_u32(line + 6, EVENT_ARCHIVE_COUNT));
else if (strncmp(line, "SNAP ", 5) == 0)  print_snapshot_record(parse_u32(line + 5, SNAP_ARCHIVE_COUNT));
```
- **默认值技巧**：`EVENT` 不跟数字时，默认值取 `EVENT_ARCHIVE_COUNT`（=16），必然 `>= COUNT` → 直接走"无有效记录"分支。省了一次判断，很巧。
- 注意 `"EVENT "` 带尾空格：所以 `EVENT` 单独输入（无参数）不会命中这个分支，而是落到 "unknown command"。属小瑕疵。

```c
else if (strcmp(line, "CLEAR") == 0) clear_metadata();
else if (strcmp(line, "QUIT") == 0) {
    if (g_running) { g_stop_request = 1U; g_quit_request = 1U; xil_printf("OK stop then quit\r\n"); }
    else g_quit_request = 1U;
}
else if (line[0] != 0) xil_printf("ERR unknown command; type HELP\r\n");
```
- `QUIT` 在运行中会**先停机再退出**（两个标志都置），顺序由主循环保证。空行不报错。

---

## 9. UART 轮询 `uart_poll`

```c
static void uart_poll(void) {
    while (XUartPs_IsReceiveData(UART_BASE)) {           // RX FIFO 里有数据就继续
        char c = (char)XUartPs_RecvByte(UART_BASE);      // 取一个字节
        if (c == '\r' || c == '\n') {                    // 回车/换行 = 一条命令结束
            if (g_uart_len != 0U) {
                g_uart_line[g_uart_len] = 0;             // 补字符串结束符
                execute_command(g_uart_line);
                g_uart_len = 0U;
            }
        } else if (c == 8 || c == 127) {                 // BS / DEL
            if (g_uart_len != 0U) --g_uart_len;
        } else if (c >= ' ' && c <= '~') {               // 可打印 ASCII
            if (g_uart_len + 1U < UART_LINE_BYTES) g_uart_line[g_uart_len++] = c;
            else { g_uart_len = 0U; xil_printf("ERR command too long\r\n"); }
        }
    }
}
```
- **为什么用轮询**：注释写了 —— "避免在建立命令协议时引入第二中断源"。这和你项目一贯的"先排除中断变量"策略一致。
- **`while` 而非 `if`**：一次调用把 FIFO 里攒的字节全部吃掉，避免命令被截断。
- **溢出保护**：`g_uart_len + 1 < 48` 才存，保证 `+1` 位置能放 `'\0'`；超长则丢弃整条并报错。
- ⚠️ **调用位置很关键**：它被放在 DMA 等待循环内部（见 §12），所以**即使 DMA 在等包，UART 也不会丢命令**。这是本设计的关键。

---

## 10. `halt_failure` —— 失败即冻结

```c
static void halt_failure(const char *why) {
    g_acq.last_ddr_status  = ddr_read(DDR_STATUS);      // 先固化现场
    g_acq.last_slot_status = ddr_read(SLOT_STATUS);
    g_acq.last_dma_status  = dma_read(S2MM_DMASR_OFFSET);
    xil_printf("ACQ_SERVICE_FAIL: %s ddr=%08x slot=%08x dma=%08x\r\n", why, ...);
    for (;;) __asm__ volatile ("wfi");                  // 停住，等人来 halt
}
```
- **"先取证、再停住"**：和你项目"真错误不得被掩盖"的原则一致，**不重试、不降级**。
- ⚠️ **但对"长跑服务"偏严**：任何一次瞬时 DMA 错误都会让整个服务死掉，且**不释放已锁的槽**。见 §13 的建议。

---

## 11. `configure_capture` —— 武装采集

```c
ddr_write(DDR_CTRL, 0U);            // 先关采集（o_err 的清零条件之一）
ddr_write(FREEZE_CTRL, 2U);         // freeze_resume，清粘滞 + 释放冻结
for (i = 0; i < SHUTDOWN_POLL_LIMIT; ++i) {
    if ((ddr_read(DDR_STATUS) & (1U | DDR_RING_ERR)) == 0U) break;   // 等 err 与 acq_en 都归零
}
if (i == SHUTDOWN_POLL_LIMIT) halt_failure("stale ring status did not clear");
```
- **为什么要先关再开**：`DDR_STATUS[5]` 是粘滞的，且 `acq_en=0` 是它的清零条件（`pd_ddr_ring_wr.v:429`）。**先关一轮、确认读回干净，再开**，这样才能保证后面看到的 `err` 是本轮新产生的，不是上一轮遗留的。
- **循环里 `1U | DDR_RING_ERR`**：同时要求 bit0(`acq_en`)=0 且 bit5(`err`)=0。

```c
ddr_write(SLOT_CTRL, (1U << 12));                                  // status_clear：清 full/cfg_err/cmd_err/req_overflow/ready
ddr_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED); // 清 drop 计数 + 使能触发
ddr_write(DDR_CTRL, 1U);                                           // 开采集
ddr_write(SLOT_CTRL, 1U);                                          // auto_snap_en = 1
ddr_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);
if ((ddr_read(SNAP_TRIG_CTRL) & (TRIG_ENABLED | TRIG_ARMED)) != (TRIG_ENABLED | TRIG_ARMED))
    halt_failure("automatic snapshot trigger did not arm");
```
- 顺序：**清状态 → 使能触发 → 开采集 → 开自动快照 → 复写触发并回读确认 armed**。
- 最后一次写 `SNAP_TRIG_CTRL` 是**幂等复写 + 回读自检**（`TRIG_ARMED` 是只读位，能读到 1 说明硬件真的武装了）。
- ⚠️ **注意 `SLOT_CTRL` 写 `1U`（只置 bit0）**：RTL 里有专门保护（`pd_ddr_axil.v:277-279`），不会因为 bit0=1 而顺带触发 lock/release。**已核验安全**。
- ⚠️ **本函数不清槽状态机**：`status_clear` 在 `pd_ddr_slot_mgr.v:124-128` **只清标志位，不清 `slot_state[]`**。见 §13 P1-2。

---

## 12. `archive_event_packet` —— 收一个事件包并归档

```c
u32 i, peaks = 0U, cycles = 0U, words = bytes / 8U;
u32 seq = g_acq.event_sequence;                 // 本次的序号
u32 index = seq % EVENT_ARCHIVE_COUNT;          // 环内位置
UINTPTR dst = EVENT_ARCHIVE_BASE + index * EVENT_ARCHIVE_STRIDE;
volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
```
- 环形寻址：**序号取模得到槽位**。`volatile` 必需（数据是 DMA 写的）。

```c
if (!bytes || bytes > RX_BUFFER_BYTES || (bytes & 7U)) halt_failure("bad DMA byte count");
```
- 三条合理性：非 0 / 不超缓冲 / **8 字节对齐**（AXIS 64 bit）。

```c
for (i = 0U; i < words; ++i) {
    u32 type = (u32)(rx[i] >> 56);              // 事件字类型恒在 [63:56]
    if (type == 0U) ++peaks;                    // 峰值事件包
    else if (type == 1U) ++cycles;              // 周期统计包
    else halt_failure("unknown event word type");
}
last_type = (u32)(rx[words - 1U] >> 56);
if (last_type != 1U) halt_failure("AXIS packet did not end in cycle word");
```
- **这是本文件里最硬的两条契约判据**：① 每个字的类型位只允许 0x00/0x01；② **末拍必须是周期统计包**。
- 依据：`pd_feature_core.v:844` 只在周期包上拉 `ev_tlast`，而 S2MM 遇 TLAST 结束传输 ⇒ "传输结束 ⟺ 帧尾到达"。
- 与 `pd_defines.vh:23-24`（`PD_EV_PEAK=0x00`/`PD_EV_CYCLE=0x01`）一致 ✅（见 §13）。
- ⚠️ **未知类型直接 `halt_failure`**：很严，但一个位翻转就把服务打死。可接受（宁可停不可错），但要知道代价。

```c
memcpy((void *)dst, (const void *)RX_BUFFER_BASE, bytes);   // RX → 归档环
Xil_DCacheFlushRange(dst, bytes);                           // 把 CPU 写的归档刷回 DDR
if (seq >= EVENT_ARCHIVE_COUNT) ++g_acq.event_overwrites;    // 覆盖计数（显式）
g_acq.event[index].sequence    = seq;
g_acq.event[index].ddr_addr    = (u32)dst;
g_acq.event[index].bytes       = bytes;
g_acq.event[index].peak_words  = peaks;
g_acq.event[index].cycle_words = cycles;
++g_acq.event_sequence;
```
- **`Flush` 的方向是对的**：这块内存是 **CPU 写的**，将来上位机（DMA/非缓存访问）要读，必须刷回 DDR。
- **`seq >= COUNT` 判覆盖**：seq 0..15 首轮不覆盖；seq=16 起每来一个覆盖一个，计数准确。

### `receive_one_packet` —— 收一个包（含可中断等待）

```c
Xil_DCacheFlushRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
XAxiDma_IntrAckIrq(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
if (XAxiDma_SimpleTransfer(&g_dma, RX_BUFFER_BASE, RX_BUFFER_BYTES, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS)
    halt_failure("DMA submit failed");
```
- **Cache 双保险**：先 Flush（清掉上一轮 CPU 可能留下的脏行）、再 **Invalidate（关键：DMA 写的是 DDR，CPU 必须作废缓存才能读到新数据）**。

```c
for (i = 0U; i < DMA_POLL_LIMIT; ++i) {
    uart_poll();                       // ★ 等待期间照常收 UART 命令
    if (g_stop_request) {              // ★ 收到 STOP 立刻停下
        XAxiDma_Reset(&g_dma);         // 复位 S2MM：主动丢弃这个未完成的包
        while (!XAxiDma_ResetIsDone(&g_dma)) { }
        return 0;
    }
    if (!XAxiDma_Busy(&g_dma, XAXIDMA_DEVICE_TO_DMA)) break;
}
```
- **这是整个服务"能停得下来"的关键**：轮询等待里嵌 `uart_poll()`，所以 DMA 在等包时 UART 仍然响应。
- **STOP 语义**："放弃当前这个不完整的包"——因为一个半包没有意义，`Reset` 是最干净的做法。

```c
dmasr = dma_read(S2MM_DMASR_OFFSET);
bytes = dma_read(S2MM_LENGTH_OFFSET);      // 完成后该寄存器 = 实际接收字节数
g_acq.last_dma_status = dmasr;
if (i == DMA_POLL_LIMIT || (dmasr & (DMASR_HALTED | DMASR_ERROR_MASK)) || !(dmasr & DMASR_IDLE)) {
    ++g_acq.dma_errors;
    halt_failure("DMA completion failed");
}
Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);   // 按"实际长度"失效
archive_event_packet(bytes);
```
- **四重判据**：超时 / Halted / 硬错误 / 非 Idle。
- `bytes` 建议加掩码 `& 0x7FFFFF`（官方例程如此），防止上表非长度位污染 —— 与本项目 DMA 文档同一条建议。

---

## 13. `archive_new_snapshot` —— 锁槽 → 归档 → 释放

```c
u32 status = ddr_read(SLOT_STATUS);
u32 seq = ddr_read(SLOT_SEQ);
...
if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR)) { ++g_acq.slot_errors; halt_failure("slot manager error"); }
if (g_acq.last_ddr_status & (DDR_COPY_ERR | DDR_CFG_ERR)) halt_failure("DDR copy error");
if (!(status & SLOT_READY) || seq == *last_slot_sequence) return;    // 没有新快照 → 直接返回
```
- 先去重：**用全局 `SLOT_SEQ` 与上次记录的值比较**，相同就是"没有新快照"。见 §13 P1-1（这是本文件最需要留意的地方）。
- ⚠️ 注意 `SLOT_READY` 是**电平保持位**（直到 `status_clear`），所以它本身不能当"有没有新快照"的判据，必须配合 `SLOT_SEQ`。

```c
slot = (status >> SLOT_LAST_SHIFT) & 0x3U;                  // 最近完成的槽号
if (!(status & (1U << slot))) halt_failure("READY without valid slot");
ddr_write(SLOT_CTRL, 1U << (4U + slot));                    // 锁：SLOT_CTRL[7:4] W1P
if (!(ddr_read(SLOT_STATUS) & (1U << (8U + slot)))) halt_failure("slot lock failed");   // 回读确认锁成功
```
- **"加锁后回读确认"是必须的**：`slot_mgr` 只在槽处于 `VALID` 时才允许加锁（`pd_ddr_slot_mgr.v:137`），否则只是置 `cmd_err`。回读能立刻发现"锁没锁上"。
- RTL 里 `SLOT_CTRL` 的写保护（`:277-279`）保证这次写**不会误清 `auto_snap_en`** ✅

```c
base  = ddr_read(SLOT_BASE_OFF(slot));
bytes = ddr_read(SLOT_LEN_OFF(slot));
slot_seq = ddr_read(SLOT_SEQ_OFF(slot));
flags = ddr_read(SLOT_FLAGS_OFF(slot));
if ((base & 7U) || (bytes & 7U) || !bytes || bytes > SNAP_ARCHIVE_STRIDE ||
    base < SNAP_SLOT_LOW || base + bytes > SNAP_SLOT_HIGH)
    halt_failure("invalid snapshot descriptor");
```
- **五重描述符校验**：对齐、非 0、不超归档步长、**下界**、**上界用哨兵**（`base + bytes <= SNAP_SLOT_HIGH`）。
- 上界写成"`+len` 关区间 ≤ 哨兵"是**对的**（`pd_ddr_defines.vh:126-130` 特意警告过：越界判据要写 `dst+len <= 哨兵`，不能写成 `<= 末字节`）✅

```c
index = g_acq.snapshot_sequence % SNAP_ARCHIVE_COUNT;
dst = SNAP_ARCHIVE_BASE + index * SNAP_ARCHIVE_STRIDE;
Xil_DCacheInvalidateRange((UINTPTR)base, bytes);            // 硬件槽是 DMA/PL 写的 → 先作废缓存
memcpy((void *)dst, (const void *)(UINTPTR)base, bytes);
Xil_DCacheFlushRange(dst, bytes);                           // 归档是 CPU 写的 → 刷回 DDR
... 填记录 ...
++g_acq.snapshot_sequence;
```
- **Cache 方向完全正确**：读硬件区前 Invalidate、写归档后 Flush。

```c
ddr_write(SLOT_CTRL, 1U << (8U + slot));                    // 释放：SLOT_CTRL[11:8] W1P
if (ddr_read(SLOT_STATUS) & (1U << (8U + slot))) halt_failure("slot release failed");
ddr_write(FREEZE_CTRL, 2U);                                 // freeze_resume：让 PL 能接受下一次事件
*last_slot_sequence = seq;
xil_printf("SNAP_ARCH seq=%u slot=%u src=%08x dst=%08x bytes=%u\r\n", ...);
```
- **释放 → 回读确认 → `freeze_resume`**：三者缺一不可。少了 `freeze_resume`，`pd_snapshot_trigger.v:51` 的 `waiting_resume` 不会清零，**后续事件永远不会再触发快照**。

---

## 14. `clean_stop` —— 干净停机

```c
u32 snapshots_before = g_acq.snapshot_sequence;
ddr_write(SNAP_TRIG_CTRL, 0U);      // 1) 先关事件触发门（不再接受新事件）
ddr_write(DDR_CTRL, 0U);            // 2) 关采集
ddr_write(FREEZE_CTRL, 2U);         // 3) 释放冻结
```
- 注释解释了顺序理由："**先关准入，再排空一个已经被接受的触发**"——因为写入 PL 需要时间，关门前可能已经有一个事件被接受。**只等 DMA/槽不忙是不够的**：一个"已完成但未归档"的槽是**有效数据**，必须搬走并释放。

```c
for (i = 0; i < SHUTDOWN_POLL_LIMIT; ++i) {
    u32 ddr = ddr_read(DDR_STATUS);
    u32 slot = ddr_read(SLOT_STATUS);
    archive_new_snapshot(last_slot_sequence);          // 尝试搬走一个残留快照
    ddr = ddr_read(DDR_STATUS); slot = ddr_read(SLOT_STATUS);   // 重新采样
    if (!(ddr & DDR_COPY_BUSY) && !(slot & SLOT_BUSY_MASK) && !(slot & SLOT_VALID_MASK)) {
        xil_printf("ACQ_CLEAN_STOP ddr=%08x slot=%08x polls=%u drained=%u\r\n", ...);
        return;
    }
}
halt_failure("capture did not quiesce");
```
- **退出条件三条同时满足**：无拷贝在途、无槽忙、**四槽全部不再 valid**。
- ⚠️ **这里有一个真实风险**：循环每次只能搬**一个**新快照（`archive_new_snapshot` 靠全局 `SLOT_SEQ` 去重），**搬完一个之后 `seq == last`，再调用就什么都不做**。所以如果停机时有 **2 个以上**未归档的 VALID 槽，`!(slot & SLOT_VALID_MASK)` 永远不成立 → 1e6 次后 `halt_failure`。详见 §13 P1-1。

---

## 15. `main`

```c
memset((void *)&g_acq, 0, sizeof(g_acq));
g_acq.magic = 0x50444151U;   /* "PDAQ" */
g_acq.version = 1U;
xil_printf("\r\n--- pd acquisition UART service V1 ---\r\n");
xil_printf("event archive=%08x x%u, snapshot archive=%08x x%u\r\n", ...);
```
- 开机先初始化共享结构体、打出版图。`0x50444151` = ASCII "PDAQ"（P=0x50, D=0x44, A=0x41, Q=0x51）—— 上位机可用这个魔数确认结构体有效。

```c
cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
if (cfg == NULL) halt_failure("DMA config not found");
if (XAxiDma_CfgInitialize(&g_dma, cfg) != XST_SUCCESS) halt_failure("DMA init failed");
if (XAxiDma_HasSg(&g_dma)) halt_failure("expected Simple-mode DMA");
XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
XAxiDma_Reset(&g_dma);
while (!XAxiDma_ResetIsDone(&g_dma)) { }
```
- 标准五步：查配置 → 初始化 → 查模式 → **关中断（纯轮询）** → 软复位并等完成。

```c
xil_printf("READY: type HELP, then START 128 (or START 0 for continuous)\r\n");
while (!g_quit_request) {
    uart_poll();                                     // 空转时也在收命令
    if (!g_running) {
        if (!g_start_request) continue;              // 没命令就继续转
        g_start_request = 0U; g_stop_request = 0U; packets = 0U;
        configure_capture();
        last_slot_sequence = ddr_read(SLOT_SEQ);     // 记录"起跑线"
        g_running = 1U;
        xil_printf("OK START limit=%u seq=%u\r\n", g_start_limit, last_slot_sequence);
        continue;
    }
    ...
}
```
- **空闲分支**：只有收到 `START` 才武装；`last_slot_sequence` 取当前 `SLOT_SEQ` 作为基准线。
- ⚠️ **副作用**：这条基准线意味着"启动前就存在的快照"不会被归档（视为已处理）。若上轮异常退出遗留了 VALID 槽，就会卡住 `clean_stop`。见 §13 P1-2。

```c
    if (g_stop_request || !receive_one_packet()) {
        archive_new_snapshot(&last_slot_sequence);   // 补搬一个
        clean_stop(&last_slot_sequence);             // 干净停机
        g_running = 0U; g_stop_request = 0U;
        xil_printf("OK STOP packets=%u events=%u snapshots=%u\r\n", ...);
        continue;
    }
    archive_new_snapshot(&last_slot_sequence);       // 正常路径：每收一个包，顺带看一眼有没有新快照
    ++packets;
    if ((packets % REPORT_PERIOD) == 0U)
        xil_printf("ACQ packets=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u drops=%u\r\n", ...);
    if (g_start_limit != 0U && packets >= g_start_limit) {
        archive_new_snapshot(&last_slot_sequence);
        clean_stop(&last_slot_sequence);
        g_running = 0U;
        xil_printf("ACQ_SERVICE_PASS packets=%u events=%u snapshots=%u ev_ovw=%u snap_ovw=%u\r\n", ...);
    }
```
- **主循环一句话**：收一个包 → 归档事件 → 顺手归档一个新快照 → 计数 → 到点打印 / 到量收工。
- **`ACQ_SERVICE_PASS` 只在 `START <n>`（有界）跑完时打印**；`START 0`（连续）永远不打 PASS —— 这是刻意的：连续模式没有"完成"的定义。

```c
xil_printf("BYE: service is quiescent.\r\n");
for (;;) __asm__ volatile ("wfi");
```
- 只有 `QUIT` 才走到这里；此时已 `clean_stop`，DMA 空闲、槽已释放 ⇒ **安全**。

---

## 16. 核验结论

### ✅ 与 RTL/契约一致（已逐条核对）

| 项 | 依据 |
|---|---|
| 13 个偏移 + 槽描述符步长 `0x10` | `pd_ddr_axil.v:134-174` |
| `DDR_STATUS` 位 `busy=1 err=5 copy_err=8 cfg_err=9` | `:316-319` |
| `SLOT_STATUS` 位 `valid[3:0]/busy[7:4]/locked[11:8]/cmd_err15/last[18:17]/ready19` | `:341-345` |
| `SNAP_TRIG_CTRL` 位 `en0/mask[4:1]/armed16` | `:348-352` |
| 事件包类型 `0x00/0x01`、类型位恒在 `[63:56]` | `pd_defines.vh:22-24`、`pd_feature_core.v:562-578` |
| "末拍必为周期包" | `pd_feature_core.v:844`（周期包 = 帧尾，拉 `ev_tlast`） |
| 加锁需 `VALID`、释放需 `LOCKED`、W1P 不回读不算成功 | `pd_ddr_slot_mgr.v:135-144` |
| 写 `SLOT_CTRL[0]` 不会误清 lock/release（有专门保护） | `pd_ddr_axil.v:277-279` |
| 槽上界用哨兵 `base+len <= 0x23001000` | `pd_ddr_defines.vh:126-130` |
| DMASR 掩码 `0x4070` = bit4/5/6/14 | PG021 |
| 新增四段内存**互不重叠、都在 1 GiB 内、避开 PL 环与硬件槽区** | 脚本实算（§4） |
| Cache 方向：读硬件区前 Invalidate、写归档后 Flush | Zynq 一致性规则 |

### ⚠️ 建议关注（按严重度）

**P1-1｜多槽同时待归档时，`clean_stop` 会卡死。**
`archive_new_snapshot` 靠**全局 `SLOT_SEQ` 与 `last_slot_sequence` 去重**，每次最多搬**一个**快照；搬完后 `seq == last`，再调用即空转。若停机时有 ≥2 个未归档的 `VALID` 槽，退出条件 `!(slot & SLOT_VALID_MASK)` 永不成立 → `SHUTDOWN_POLL_LIMIT` 后 `halt_failure("capture did not quiesce")`。
**建议**：把排空改成**按槽遍历**——对 4 个槽逐个 `lock → 读描述符 → 归档 → release`，不要依赖全局 `SLOT_SEQ` 判断"有没有新的"。

**P1-2｜上一轮遗留的 `VALID` 槽会污染新一轮启动。**
`configure_capture` 只写 `SLOT_CTRL[12]`（`status_clear`），而该位在 `pd_ddr_slot_mgr.v:124-128` **只清标志位、不复位 `slot_state[]`**；再加上 `last_slot_sequence = ddr_read(SLOT_SEQ)` 把"启动前已完成的快照"视为已处理 → 这些旧槽永远不会被搬走 → 同样卡死 `clean_stop`。
**建议**：启动时先按槽遍历清空（对每个 `VALID` 槽 lock+release），或在 RTL 侧让 `status_clear` 一并复位槽状态（**改 RTL 前要先过文档**）。

**P2-1｜`halt_failure` 对长跑服务偏严。**
一次瞬时 DMA 错误 → 整个服务 `wfi` 永久停住，且**不释放已锁的槽**。建议至少先走一遍"释放槽 + 关触发"，或区分"致命/可恢复"。

**P2-2｜事件归档步长余量只有 8 字节。**
`EVENT_ARCHIVE_STRIDE = 65,536`，`RX_BUFFER_BYTES = 65,528`。当前安全（超限会 `halt`），但一字节都不富余。建议 stride 提到 96 KiB，或写入前显式断言 `bytes <= EVENT_ARCHIVE_STRIDE`。

**P2-3｜注释与事实相反的措辞。**
`RX_BUFFER_BASE` 注释说"在 PL 原始环形区之后"，实际在 `0x0110_0000`，**低于**环形区（`0x1000_2000`）。不影响正确性。

**P2-4｜`EVENT`（不带数字）落到 "unknown command"。**
因为匹配串是 `"EVENT "`（带空格）。要么改成 `"EVENT"` + 边界检查，要么在 HELP 里说明必须带参数。

**P2-5｜`SLOT_LOCKED_MASK` 定义了但全文未使用**（无害）。
