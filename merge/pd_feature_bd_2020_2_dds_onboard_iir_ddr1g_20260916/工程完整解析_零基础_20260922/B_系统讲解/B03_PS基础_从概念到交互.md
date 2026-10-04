# B03 · PS 基础：从概念到交互

> 读者对象：完全没接触过 Zynq、没写过 ARM、没碰过 AXI 的初学者。
> 本文所有"功能事实"都来自本工程代码（给出 文件:行号），设计意图参考根目录 `.md` 文档。
> 配套文档：B04_系统架构与模块划分.md（讲 PL 内部与整体框图）。

---

## 0. 先建立一张"心智地图"

本工程是一个**局部放电（PD）检测系统**。核心任务只有一句话：
**用 FPGA（PL）把 4 路 ADC 采样数据做实时的特征提取，再把"原始数据 + 特征值"写进 DDR 内存，最后由 ARM 处理器（PS）通过网口/TTL 串口把结果发给上位机。**

你只要记住三个角色：
- **PL（Programmable Logic）** = 那片 FPGA 逻辑，干"重体力活"（高速采样、滤波、打包、搬数据到 DDR）。
- **PS（Processing System）** = 一片 ARM Cortex-A9 双核，干"脑力活"（接收命令、调度采集、把数据通过网络发出去）。
- **DDR** = 一块外接的内存条，PL 和 PS 都能读写，是它们交换数据的"公共黑板"。

下面各节先给直觉/比喻，再给严格解释，最后落到本工程真实代码。每一节末尾有"如果这里搞错，会表现为……"。

---

## 1. Zynq 是什么：为什么叫"全可编程 SoC"，为什么不把 PS/PL 看成两颗芯片

### 直觉/比喻
把 Zynq 想象成一栋**同一块硅片上**盖好的"厂房 + 办公室"：
- 厂房（PL）是你可以随意重新布线的车间，今天让它做滤波、明天让它做加密都行；
- 办公室（PS）是一栋已经装修好的标准办公楼，里面有现成的电脑（CPU）、仓库管理员（DDR 控制器）、收发室（网口/串口）；
- 厂房和办公室之间，**在同一栋楼里就修好了好几条内部专用货梯/传送带（AXI 总线）**，所以它们之间搬货极快，不需要走外部马路。

如果换成"两颗独立芯片"（比如一片 FPGA + 一片外置 ARM），它们之间要用 PCB 板上的引脚连起来，带宽低、延迟大、还容易受干扰。Zynq 把两者做进同一颗芯片，**物理上 inseparable**，所以叫 **SoC（System on Chip，片上系统）**；又因为 PL 可被"重新编程"（bitstream 决定它干什么），PS 上的 ARM 也能跑不同软件，所以叫 **"全可编程（All Programmable）SoC**。

### 严格解释（本工程）
本工程用的芯片是 **Xilinx Zynq-7020**（器件 `xc7z020clg484-2`，见 `pd_defines.vh:3`）。在 Block Design 里，PS 这一侧对应的 IP 实例叫 `processing_system7_0`，其 VLNV 为 `xilinx.com:ip:processing_system7:5.5`（`.bd` 的 `components` 段）。

关键点：**PL 不是"挂在 PS 外面"的器件，而是和 PS 通过片内 AXI 互联矩阵直接相连**。PS 既可以作为"主设备"去访问 PL 里的寄存器，也可以作为"从设备"让 PL（DMA）直接往它的 DDR 里写数据。因此本文绝不把 PS、PL 当成两颗芯片来分别调试——它们共享同一地址空间、同一块 DDR。

### 本工程证据
- PS 实例：`processing_system7_0`（`pd_feature_bd.bd` → `design.components`）。
- PS 打开了 GP0 主口（PS 主动访问 PL 寄存器）：`PCW_USE_M_AXI_GP0 = 1`（`pd_feature_bd.bd` → `processing_system7_0` 参数）。
- PS 打开了 HP0/HP1 从口（PL 的 DMA 主动写 PS 的 DDR）：`PCW_USE_S_AXI_HP0 = 1`、`PCW_USE_S_AXI_HP1 = 1`。
- 中断：PL → PS 的中断已打开：`PCW_USE_FABRIC_INTERRUPT = 1`、`PCW_IRQ_F2P_INTR = 1`、`PCW_IRQ_F2P_MODE = DIRECT`。

> **如果这里搞错，会表现为：** 把 PL 当成"外设芯片"去查它的独立 datasheet，结果找不到任何对外引脚协议——因为 PL 的寄存器其实是被 PS 通过片内 AXI 总线按"内存地址"读写的；调试时若用示波器去量"PS 访问 PL 寄存器的总线波形"，也量不到，因为那是片内走线。

---

## 2. PS 内部有什么：CPU、Cache、OCM、DDR 控制器、外设、AXI 互联、GIC

### 直觉/比喻
把 PS 想象成办公楼的管理层：
- **CPU（双核 Cortex-A9）**：两个办事员，跑你的 C 程序。
- **L1/L2 Cache（缓存）**：办事员手边的便签纸，CPU 读写 DDR 时先在这上面记一笔，下次同地址直接看便签，省得跑仓库。
- **OCM（On-Chip Memory，片上存储器）**：楼内的小型保险柜，极快但很小，放紧急数据。
- **DDR 控制器**：仓库管理员，负责和楼外那块大内存条（DDR3）对话。
- **外设（UART/ENET/USB/SD/QSPI）**：收发室、网口、U 盘接口、SD 卡槽、Flash 启动芯片。
- **AXI 互联**：楼内的一组内部电梯/走廊，分三种规格（见下）。
- **GIC（Generic Interrupt Controller，中断控制器）**：总机，谁有急事就打电话给 CPU。

### 严格解释（对照本工程配置）
从 `.bd` 里 `processing_system7_0` 的参数可以逐条确认 PS 内部启用了什么：

| PS 内部部件 | 本工程配置 | 在 `.bd` 中的位置 |
|---|---|---|
| CPU 主频 | APU 666.666 MHz（`PCW_APU_PERIPHERAL_FREQMHZ`） | `processing_system7_0` 参数 |
| L1/L2 | Cortex-A9 双核自带，6:2:1 比例（`PCW_APU_CLK_RATIO_ENABLE=6:2:1`） | 同上 |
| DDR 控制器 | 启用（`PCW_EN_DDR=1`），DDR3 533 MHz（`PCW_UIPARAM_DDR_FREQ_MHZ`），32 位宽（`PCW_UIPARAM_DDR_BUS_WIDTH=32 Bit`），型号 MT41K256M16（1GB） | 同上 |
| UART0 | 启用（`PCW_EN_UART0=1`），接 MIO 14..15，波特率 115200（`PCW_UART0_BAUD_RATE`） | 同上 |
| ENET0 | 启用（`PCW_EN_ENET0=1`），接 MIO 16..27 + MDIO MIO 52..53，千兆 | 同上 |
| TTC0 | 启用（`PCW_EN_TTC0=1`，EMIO 引出） | 同上 |
| FCLK0 | 150 MHz（`PCW_FPGA0_PERIPHERAL_FREQMHZ=150`），作为 PS 给 PL 的输出时钟之一 | 同上 |

**三种 AXI 口（这是 PS 与 PL 通信的全部通道，务必分清）：**
1. **GP 口（General Purpose，通用）**：PS 当"主"，PL 当"从"。本工程只开了 **M_AXI_GP0**（PS 访问 PL 寄存器用），数据位宽 32 位（`PCW_M_AXI_GP0_ID_WIDTH=12`）。比喻：CPU 亲自下车间去拧 PL 的"旋钮"（写寄存器）、看 PL 的"仪表盘"（读寄存器）。
2. **HP 口（High Performance，高性能）**：PS 当"从"，PL 当"主"。本工程开了 **S_AXI_HP0 / S_AXI_HP1**，都是 64 位宽（`PCW_S_AXI_HP0_DATA_WIDTH=64`）。比喻：车间（PL）用大卡车（DMA）直接把货（数据）送进仓库（DDR），不走 CPU。
3. **ACP 口（Accelerator Coherency Port）**：本工程**未启用**（`PCW_USE_S_AXI_ACP=0`），本文不展开。

> **如果这里搞错，会表现为：** 把 HP 口当成"PS 主动写数据给 PL"的口，结果怎么写都不通——因为 HP 的方向是 PL→PS(DDR)，PS 主动搬数据要用的是 GP0（控制）或 ACP。本工程 PL→DDR 全走 HP，CPU 只通过 GP0 发命令。

---

## 3. 地址空间：CPU 怎么访问 PL 里的寄存器（含本工程真实地址）

### 直觉/比喻
PS 把整块芯片（包括 PL 里的寄存器、自己的 DDR、自己的外设）看成一条**很长的街道，门牌号就是地址**。CPU 说"去 0x4000_0000 号房子读一个 32 位数"，总线就会把请求送到那栋房子（这里是 `pd_ddr_0` 模块的寄存器）。写寄存器就是"往某个门牌号塞一个值"。

### 严格解释（本工程真实地址，来自 `.bd` 的 `addressing` 段）
本工程的地址映射是 Vivado 在 BD 里自动分配的，解析 `.bd` 可得（`.bd` → `design.addressing`）：

| 目标 | 基地址 | 范围 | 作用 |
|---|---|---|---|
| `pd_ddr_0` | `0x4000_0000` | 64K | DDR 环形缓存/快照管理 IP 的 AXI-Lite 寄存器 |
| `pd_feature_0` | `0x4001_0000` | 64K | 特征提取 IP 的 AXI-Lite 寄存器（配置/状态/PRPD 图谱） |
| `pd_filter_0` | `0x4002_0000` | 64K | IIR 滤波器 IP 的 AXI-Lite 寄存器（系数/旁路） |
| `axi_dma_0` | `0x4040_0000` | 64K | AXI DMA（S2MM）寄存器（启动一次 DMA 搬运） |
| DDR（内存） | `0x0000_0000` | 1G（`range: 1G`） | 大内存，PL 的 DMA 和 CPU 都能读写 |

补充说明（来自 `.bd` 参数）：PS 自己保留的安全可用内存从 `0x0010_0000` 起到 `0x3FFF_FFFF`（`PCW_DDR_RAM_BASEADDR`/`HIGHADDR`）。本工程软件里 `PD_PS_DDR_BASE` 取 `XPAR_PS7_DDR_0_BASEADDRESS`，再往下划分出 RX 缓冲、事件归档区、快照归档区、四槽快照区（见 `pd_hw_map.h:78-87`）。

**CPU 怎么"访问 PL 里的寄存器"——本工程的真实代码：**
软件侧封装了两个小函数（`sw/ps_service/src/pd_acquisition_core.c:41-43`）：
```c
static u32 ddr_read (u32 off) { return Xil_In32 (PD_DDR_BASE + off); }   // 读
static void ddr_write(u32 off, u32 value) { Xil_Out32(PD_DDR_BASE + off, value); } // 写
```
其中 `PD_DDR_BASE` 就是 `0x4000_0000`（由当前 `xparameters.h` 的 `XPAR_PD_DDR_0_BASEADDR` 得到）。例如"启动采集"就是 `ddr_write(PD_DDR_CTRL, 1U)`（写 `0x4000_0000 + 0x000`），"读状态"就是 `ddr_read(PD_DDR_STATUS)`（读 `0x4000_0004`）。CPU 不需要知道 PL 内部细节，它只认"地址 + 32 位值"。

> **如果这里搞错，会表现为：** 把 `0x4000_0000`、`0x4001_0000`、`0x4002_0000` 三个相邻 64K 地址窗里的从机基址/偏移算错或越界，写入会落到别的模块寄存器上，现象是"写 A 模块的配置，B 模块的行为却变了"，极难排查；或者 bit 文件与软件来自不同版本，寄存器布局对不上，读写全是乱码。

---

## 4. 启动流程：BootROM → FSBL → PL 配置（bitstream）→ 跳转到应用程序

### 直觉/比喻
Zynq 上电后，**PS 这一侧先醒**，流程像"开公司"：
1. **BootROM（只读启动程序，芯片出厂固化）**：相当于"门卫手册"，上电第一件事，决定从哪启动（QSPI Flash / SD 卡 / JTAG）。
2. **FSBL（First Stage Boot Loader，第一阶段引导）**：相当于"开业筹备组"，它干两件大事——① 把 PL 的配置比特流（bitstream）**下载进 PL**，把那片"空白车间"按图纸布置好；② 把你的应用程序（ELF）搬进 DDR 并跳过去执行。
3. **应用程序（你的 `main()`）**：正式营业。

### 严格解释 + 为什么"bit 与 ELF 必须同源"
- PL 上电时是"空白 FPGA"，里面没有任何逻辑。它必须由 bitstream 配置出 `pd_feature_0`、`pd_ddr_0`、`pd_filter_0`、`axi_dma_0` 这些模块，以及它们的寄存器布局、中断号、地址映射。
- PS 上的软件（ELF）是用 `Xil_In32/Xil_Out32` 按**固定地址和固定位定义**去读写这些寄存器的（例如 `pd_hw_map.h` 里 `PD_DDR_CTRL=0x000`、`PD_DDR_STATUS=0x004`）。
- 所以：**bit 决定了硬件契约（寄存器在哪、每位什么意思），ELF 依赖这个契约。** 如果 bit 来自版本 A、ELF 来自版本 B，寄存器布局对不上，软件读写就会错位、误触发故障。**bit 与 ELF 必须来自同一次工程编译（同一份 `xparameters.h` / 同一个 `.bd`）。** 本工程的 `pd_feature_bd_wrapper.xsa`（含 bit + 硬件描述）正是用来生成配套 BSP/软件的（`sw/` 下的代码都 `#include "xparameters.h"`）。

> **如果这里搞错，会表现为：** 单独更新了软件 ELF 却没重新生成 bit（或反之），板子能启动但一采集就进 `PD_ACQ_FAULT`（见 `pd_acquisition_core.c:45-56` 的 `fail()`），或者寄存器读写值永远对不上，状态机卡死。

---

## 5. 裸机（bare-metal）编程模型：没有操作系统；main() + 死循环；Xil_In32/Xil_Out32；为什么用 xil_printf；volatile；wfi

### 直觉/比喻
"裸机"就是**没有 Windows、没有 Linux**，CPU 一上电就直接跑你写的 C 程序。没有"操作系统帮你调度任务、帮你打印、帮你管理文件"。你必须自己写 `main()`，写完之后让 CPU 原地"待命/轮询"，不能让 `main()` 返回（返回后没地方可去，会跑飞）。

### 严格解释（本工程真实代码）
**1) `main()` + 死循环**：本工程有两个入口，任选其一编译进 ELF：
- UART 版本 `sw/ps_service/src/main.c:120-132`：
  ```c
  int main(void) {
      if (pd_acq_init() != XST_SUCCESS) { ... for(;;) __asm__ volatile ("wfi"); }
      for (;;) { uart_poll(); (void)pd_acq_poll(); }   // 死循环：轮询串口 + 轮询采集
  }
  ```
- TCP 版本 `sw/ps_service/tcp/main.c:44`：`for (;;) (void)pd_acq_poll();`

**2) 寄存器读写用 `Xil_In32` / `Xil_Out32`**：见 `pd_acquisition_core.c:41-43`，底层就是对一个地址做 32 位读/写，没有任何操作系统介入（裸机直接访存）。

**3) 为什么不能用 `printf`，要用 `xil_printf`**：在裸机里没有标准 C 库的文件系统/stdout 后端。`printf` 会链接一大堆不可用的 I/O 桩，要么编译不过，要么运行时卡死。`xil_printf` 是 Xilinx 提供的"精简版"，直接往 UART 串口吐字符（`sw/ps_service/src/main.c:7` 包含 `xil_printf.h`，全文都用 `xil_printf` 而非 `printf`）。

**4) `volatile` 的作用**：本工程全局状态结构体被声明为 `volatile pd_acq_shared_t g_pd_acq;`（`pd_acquisition_core.c:27`）。`volatile` 告诉编译器"这个变量可能被硬件/中断/其他地方随时改，别把它缓存在寄存器里、别自作主张优化掉读写"。对"被轮询循环反复读、且值会被别的代码（如接收回调）改"的变量，必须加 `volatile`，否则编译器可能认为"反正我没改它"而永远用旧值。

**5) `wfi` 指令（Wait For Interrupt，原地待命）**：`__asm__ volatile ("wfi")`（`main.c:124`、`:30`、tcp `main.c:30,39`）。当初始化失败时，CPU 无事可做，用 `wfi` 让 CPU 进入低功耗待机，避免空转发热/跑飞。正常工作时是死循环轮询，不依赖中断。

> **如果这里搞错，会表现为：** 误用 `printf` → 编译报 undefined reference 或板子启动即跑飞；漏写 `volatile` → 轮询状态永远读到旧值，软件以为"采集没完成"或"没新数据"，死等；`main()` 返回 → CPU 跑飞，串口输出乱码或完全静默。

---

## 6. Cache 一致性：为什么 PL 经 DMA 写进 DDR 的数据，CPU 可能读到旧值；正确的 Invalidate / Flush 顺序

### 直觉/比喻（本项目最容易出错的地方）
回忆第 2 节的"便签纸"比喻：CPU 读 DDR 时会先把数据抄到便签（Cache）上，下次同地址直接看便签。现在出现一个情况——**车间（PL 的 DMA）绕开 CPU，直接往仓库（DDR）的一格写了新货**。但 CPU 手边的便签上还记着"这一格是旧货"。于是 CPU 一看便签，读到的还是旧值。这就是 **Cache 一致性问题**。

- **Invalidate（使无效 / 作废便签）**：把便签上对应那一格"撕掉"，强制 CPU 下次去仓库（DDR）重新拿。→ 用于"PL 刚写了 DDR，CPU 现在要读"之前。
- **Flush（冲刷 / 把便签写回仓库）**：CPU 在便签上改了东西，主动把它写回仓库，保证仓库里是最新的。→ 用于"CPU 刚写了 DDR，别人（PL/DMA 或网络）要读"之前。

### 严格解释（本工程真实调用，带 file:line）
本工程里 CPU 和 PL 共享 DDR 的三个区域，每一处都严格配对 Invalidate/Flush：

**场景 A —— 快照拷贝：PL 把原始数据写进 DDR 的"四槽快照区"，CPU 要读它来做分析**（`pd_acquisition_core.c:143-147`）：
```c
index = g_pd_acq.snapshot_sequence % PD_SNAP_ARCHIVE_COUNT;
dst   = PD_SNAP_ARCHIVE_BASE + index * PD_SNAP_ARCHIVE_STRIDE;
Xil_DCacheInvalidateRange((UINTPTR)base, bytes);          // ① 先作废：base 处是 PL 写的新数据，CPU 旧便签作废
memcpy((void *)dst, (const void *)(UINTPTR)base, bytes);  // ② CPU 读取（此时必读到 PL 新值）
Xil_DCacheFlushRange(dst, bytes);                         // ③ 冲刷：dst 是 CPU 刚写的分析归档区，保证落地到 DDR
```

**场景 B —— DMA 事件包：PL 的 AXI DMA（S2MM）把事件流写进 RX 缓冲，CPU 要读它来归档**（`pd_acquisition_core.c:274`）：
```c
Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, bytes);      // DMA 刚写完，作废 CPU 便签再读
if (archive_event_packet(bytes) != XST_SUCCESS) ...
```
而在**启动一次 DMA 之前**，CPU 还要先清掉自己可能残留的脏便签、并作废旧缓存，避免 DMA 写入后 CPU 旧便签又把错误数据写回（`pd_acquisition_core.c:169-170`）：
```c
Xil_DCacheFlushRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES);    // 先冲刷：清掉 CPU 可能留下的脏行
Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES); // 再作废：等 DMA 写完后 CPU 会重新读
```

**网络发送前也要作废**（TCP 把 DDR 里的数据发出去，必须保证发的是 DDR 真值，不是 CPU 便签旧值）：`pd_tcp_service.c:121`（`transfer_pump`）、`:603`（GET 命令前）、`:255`、`:400`（FFT 分析前）。

**正确顺序的口诀**：
- **要读"别人（PL/DMA）刚写的内存" → 先 `Invalidate`，再读。**
- **自己（CPU）刚写了内存、要让别人（PL/网络）读 → 先 `Flush`。**
- 两者方向不同，**不能反**，也不能漏。

> **如果这里搞错，会表现为：** 漏掉 `InvalidateRange` → CPU 读到的是 DDR 里几天前（或上一包）的旧数据，现象是"明明 PL 在持续放电，上位机收到的快照/事件却是静止的或错乱的"，且时好时坏（取决于 Cache 那一行有没有恰好被替换）；漏掉 `FlushRange` → 你分析/改好的数据没真正落盘，网络发出去的是 DDR 里的旧垃圾。

---

## 7. 中断 vs 轮询：GIC 是什么、IRQ_F2P 是什么（xlconcat_0 把 3 个中断合成一路）；为什么本工程选择轮询

### 直觉/比喻
- **中断（Interrupt）**：相当于"电话铃响 CPU 才去处理"，CPU 平时可以干别的（或睡觉 `wfi`），有急事硬件打 CPU 的"总机（GIC）"。
- **轮询（Polling）**：相当于"CPU 每隔一会儿就亲自去门口看一眼信箱有没有新信"，不管有没有事都要跑一趟。

### 严格解释（本工程真实连线）
- **GIC（Generic Interrupt Controller）**：PS 内部的总机，负责把各种中断（定时器、UART、以及来自 PL 的 `IRQ_F2P`）汇总后通知 CPU 核。
- **`IRQ_F2P`（Fabric to PS Interrupt）**：PL 这侧的" Fabric "可以向 PS 发起的中断线。本工程打开了它且为 `DIRECT` 模式（`PCW_IRQ_F2P_INTR=1`、`PCW_IRQ_F2P_MODE=DIRECT`）。
- **`xlconcat_0` 把 3 个中断合成一路**：本工程有 3 个 PL 中断源，但 PS 只有有限的 `IRQ_F2P` 线。BD 里用 `xlconcat_0`（`NUM_PORTS=3`）把三路合成一个 `dout` 接到 `processing_system7_0/IRQ_F2P`（`.bd` 的 `interface_nets` / `nets`）：
  - `In0` ← `pd_feature_0/irq`（特征事件中断）
  - `In1` ← `axi_dma_0/s2mm_introut`（DMA 完成中断）
  - `In2` ← `pd_ddr_0/irq`（快照/环形缓存中断）
  - 三路经过 `xlconcat_0` 合成 `xlconcat_0/dout` 一路送给 PS（`.bd` → `nets.xlconcat_0_dout`）。

### 为什么本工程选择"轮询"而不是"中断"
本工程主程序是**死循环轮询**（`main.c:128-131` 里 `uart_poll()` + `pd_acq_poll()`；TCP 版 `tcp/main.c:44` 里 `pd_acq_poll()`），并没有安装 GIC 中断处理函数。原因写在测试程序的注释里（`sw/pd_snapshot_poll.c:4-8`）：

> *"This test intentionally polls pd_ddr_0 instead of installing a GIC handler. The BD already routes pd_ddr_0/irq to IRQ_F2P[2], but the generated BSP does not expose a named pd_ddr interrupt macro yet. Polling therefore verifies the complete PL/DDR/AXI-Lite contract without guessing an interrupt ID."*

翻译要点：**硬件连线（IRQ_F2P[2]）是好的，但自动生成的 BSP 还没给 `pd_ddr` 分配一个"有名的中断号宏"，为了避免去猜中断 ID 导致装错 handler，先用轮询把整条 PL→DDR→AXI-Lite 通路验证通。** 轮询虽然"占 CPU"，但对于本工程这种"采集循环本就在死循环里跑"的结构，反而更简单、更不容易出错。

> **如果这里搞错，会表现为（若强行上中断却 ID 配错）：** 中断永远不进 handler，或者进错 handler（把 DMA 完成当成特征中断），采集停滞；而本工程用轮询避免了这点，代价是 `pd_acq_poll()` 每圈都要 `Xil_In32` 读状态寄存器，CPU 不能深度睡眠。

---

## 8. lwIP 与 TCP 服务：RAW 模式为什么需要周期调用 tcp_fasttmr/tcp_slowtmr/xemacif_input；struct tcp_pcb 与回调；tcp_write/tcp_output/tcp_sndbuf

### 直觉/比喻
- **lwIP**：一个"迷你网络协议栈"（跑在裸机/嵌入式上，没有完整操作系统也能用），负责把你的数据打包成 TCP/IP 包发出去、把收到的包拆开给你。
- **RAW 模式**：lwIP 的一种"裸机用法"。比喻：没有专门的"网络线程"帮你收发，你（主循环）必须**定期亲自去摇几下铃铛**，协议栈才能推进超时重传、维持连接。
- **网络包 / 发送缓冲 / 握手**的比喻：两个电脑像两个人打电话——"握手"是拨号接通（三次握手），"网络包"是一句句话，"发送缓冲（sndbuf）"是你要说的话先排在嘴边的小本子上，本子写满了就得等对方确认收到（ACK）腾出位置才能继续写。

### 严格解释（本工程真实代码）
**1) 为什么 RAW 模式要周期调用三个函数**（`sw/ps_service/tcp/pd_tcp_service.c:767-773`）：
```c
void pd_tcp_service_poll(void) {
    if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }   // 快速定时器（约 250ms 级）：重传、保活
    if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }   // 慢速定时器（约 500ms 级）：连接超时
    xemacif_input(&echo_netif);                                  // 把网卡收到的新包喂给协议栈
    transfer_pump();                                             // 把待发数据分块塞进发送缓冲
}
```
这三个函数是 lwIP RAW 模式的"心跳"：因为没有操作系统定时帮它跑，**必须由主循环反复调用**。旗标 `TcpFastTmrFlag/TcpSlowTmrFlag` 由底层定时器中断置位（Xilinx 模板），主循环看到旗标就跑一次对应的 `tmr`。`xemacif_input` 是 `xemacif` 驱动提供的"把 EMAC 收到的帧交给 lwIP"的入口。本工程在 `tcp/main.c:41` 把这个函数注册成采集循环的"轮询钩子"（`pd_acq_set_poll_hook(pd_tcp_service_poll)`），于是每次 `pd_acq_poll()` 都会顺带推进网络栈。

**2) `struct tcp_pcb` 与回调**（本工程在 `pd_tcp_service.c`）：
- `struct tcp_pcb *s_client`：一个"已建立的 TCP 连接"的句柄（`pd_tcp_service.c:33`）。
- **accept 回调** `accept_cb`（`pd_tcp_service.c:738-752`）：有新客户端连上来时触发，记录 `s_client = pcb`，并挂上 recv/err 回调。
- **recv 回调** `receive_cb`（`pd_tcp_service.c:698-727`）：对方发来一行命令（以 `\r`/`\n` 结尾）时触发，调用 `execute_command()` 解析执行（START/STOP/STATUS/GET/FFT…）。
- **err 回调** `error_cb`（`pd_tcp_service.c:729-736`）：连接出错时把 `s_client` 清 NULL。
- 监听端口 6001（`PD_TCP_PORT=6001`，`pd_tcp_service.c:22`；`tcp_bind` 在 `:759`，`tcp_listen` 在 `:761`）。

**3) `tcp_write` / `tcp_output` / `tcp_sndbuf`**（发送时先检查缓冲够不够，再写、再推）：
```c
// reply() —— 发一行文本响应（pd_tcp_service.c:49-61）
if (tcp_sndbuf(s_client) < len) return -1;                    // ① 先问"嘴边小本子"还有没有空位
err = tcp_write(s_client, text, len, TCP_WRITE_FLAG_COPY);    // ② 把数据写进发送缓冲（COPY 方式）
if (err != ERR_OK) return -1;
(void)tcp_output(s_client);                                   // ③ 立刻尝试把缓冲推到网卡发出去
```
大数据分块发送见 `transfer_pump()`（`pd_tcp_service.c:113-134`）：每次最多发 `PD_TCP_SEND_BYTES=1024` 字节，且同样先用 `tcp_sndbuf` 确认有空间，再 `tcp_write` + `tcp_output`，循环把一整段快照/事件发出去。

> **如果这里搞错，会表现为：** 忘了在主循环里周期调用 `tcp_fasttmr/tcp_slowtmr/xemacif_input` → 连接能建立但一发数据就卡死、或长时间无通信后连接被悄悄断开；不检查 `tcp_sndbuf` 就 `tcp_write` → 缓冲满时返回 `ERR_MEM`，数据丢失、对端收不全。

---

## 9. 本工程 PS 与 PL 的完整交互清单（对照 pd_hw_map.h 逐条可查）

下面这张表把"PS 通过 GP0 访问的每个寄存器、读什么/写什么/什么时机"列清楚。所有偏移都来自 `sw/ps_service/include/pd_hw_map.h`，基地址来自 `.bd` 的 `addressing` 段。

### 9.1 `pd_ddr_0` 基地址 `0x4000_0000`（控制/状态/快照管理）
来源：`pd_hw_map.h:40-67`、`pd_acquisition_core.c` 全程调用。

| 寄存器（宏） | 偏移 | 方向 | PS 读/写什么 | 时机（代码位置） |
|---|---|---|---|---|
| `PD_DDR_CTRL` | `0x000` | 写 | 整体使能/停止环形采集 | `pd_acquisition_core.c:61` 写 0 关；`:71` 写 1 开 |
| `PD_DDR_STATUS` | `0x004` | 读 | 环形写忙、环错、拷贝错、配置错位 | `:48, :64, :115, :197` 等多处读 |
| `PD_FREEZE_CTRL` | `0x034` | 写 | 冻结/恢复环形写（值 2 = 恢复） | `:62, :160, :194, :325` |
| `PD_SLOT_CTRL` | `0x04C` | 写 | 槽分配使能 / 锁槽(bit4+n) / 释槽(bit8+n) | `:69, :72, :132, :158, :338, :341` |
| `PD_SLOT_STATUS` | `0x050` | 读 | 4 槽 valid/busy/locked 位图、READY、LAST | `:110, :117, :123, :126, :133, :159` |
| `PD_SLOT_SEQ` | `0x054` | 读 | 快照序列号（判断是否有新快照） | `:111, :231, :353` |
| `PD_SLOT_DROPS` | `0x058` | 读 | 丢弃计数（诊断） | `pd_acquisition_core.c` 之外，`main.c:43` 打印 |
| `PD_SNAP_TRIG_CTRL` | `0x05C` | 写/读 | 自动快照触发使能/屏蔽/armed | `:70, :73, :74, :192, :322` |
| `PD_SLOT_BASE_OFF(n)` | `0x060+n*0x10` | 读 | 第 n 槽在 DDR 里的起始地址 | `:135`（读 base） |
| `PD_SLOT_LEN_OFF(n)` | `0x064+n*0x10` | 读 | 第 n 槽长度（字节） | `:136` |
| `PD_SLOT_SEQ_OFF(n)` | `0x068+n*0x10` | 读 | 第 n 槽硬件序列号 | `:137` |
| `PD_SLOT_FLAGS_OFF(n)` | `0x06C+n*0x10` | 读 | 第 n 槽标志位 | `:138` |

辅助位定义（用于解析上面的寄存器）：`PD_DDR_COPY_BUSY=(1<<1)`、`PD_DDR_RING_ERR=(1<<5)`、`PD_DDR_COPY_ERR=(1<<8)`、`PD_DDR_CFG_ERR=(1<<9)`、`PD_SLOT_READY=(1<<19)`、`PD_TRIG_ENABLED=(1<<0)`、`PD_TRIG_ARMED=(1<<16)`（均见 `pd_hw_map.h:53-67`）。

### 9.2 `axi_dma_0` 基地址 `0x4040_0000`（启动一次 DMA 把事件流搬进 DDR）
来源：`pd_hw_map.h:69-75`、`pd_acquisition_core.c:43, :266-273`。

| 寄存器（宏） | 偏移 | 方向 | 内容 | 时机 |
|---|---|---|---|---|
| `PD_S2MM_DMACR_OFFSET` | `0x30` | 写 | S2MM 控制（启动/复位/中断使能） | `pd_acquisition_core.c` 初始化（`pd_acq_init` 用 `XAxiDma_*` API 间接写） |
| `PD_S2MM_DMASR_OFFSET` | `0x34` | 读 | 状态：HALTED(bit0)/IDLE(bit1)/错误位 | `:50, :266, :270` |
| `PD_S2MM_LENGTH_OFFSET` | `0x58` | 写/读 | 本次要搬的字节数 / 实际搬完的字节数 | `:172` 经 `XAxiDma_SimpleTransfer` 写；`:267` 读回实际长度 |

DMA 的方向是 **PL(事件 AXI-Stream) → DDR(RX 缓冲)**（`c_include_s2mm=1, c_include_mm2s=0`，`.bd` 参数），数据位宽 64 位（`c_m_axi_s2mm_data_width=64`）。CPU 不参与搬运，只负责"下令 + 等完成 + 作废 Cache 后读"。

### 9.3 `pd_feature_0` 基地址 `0x4001_0000` 与 `pd_filter_0` 基地址 `0x4002_0000`
这两个 IP 也是经 GP0 的 AXI-Lite 从机（`pd_feature_sys_top.v:56` 的 `S_AXI`、`.bd` 里 `axi_ic_ctrl_M00_AXI→axi_rs_feature_ctrl_0→pd_feature_0/s_axi`、`axi_ic_ctrl_M03_AXI→pd_filter_0/S_AXI`）。它们的**详细寄存器偏移**写在各自 RTL 里（`pd_axil_regs.v`、`pd_filter_chain.v`），软件侧本工程主要通过 `pd_feature_0` 的全局控制/状态字与 PRPD 图谱读取来交互；`pd_filter_0` 则通过写系数/旁路位（`pd_filter_chain.v:89-94`：写 `0x0` 控旁路、写 `0x10+` 写系数、读 `0x00C` 可读回 `SAMPLE_HZ`）来配置 IIR 滤波。本工程 `pd_filter_apply.c` 这类脚本即用来装载滤波系数。

### 9.4 内存区域（PS 与 PL 共享的 DDR 布局，来自 `pd_hw_map.h:78-87`）
| 区域 | 地址 | 用途 |
|---|---|---|
| `PD_RX_BUFFER_BASE` | `PD_PS_DDR_BASE + 0x01000000` | DMA 把事件流写到这里，CPU 读它归档 |
| `PD_EVENT_ARCHIVE_BASE` | `0x27000000` | 事件归档区（2048 份，每份 `0x10000`） |
| `PD_SNAP_ARCHIVE_BASE` | `0x24000000` | 快照归档区（4 份，每份 `0xC00000`） |
| `PD_SNAP_SLOT_LOW/HIGH` | `0x20001000` / `0x23001000` | PL 硬件四槽快照区（PL 经 HP0 写、CPU 经 HP 读） |

> **如果这里搞错，会表现为：** 把 `PD_SLOT_BASE_OFF` 的 `n` 算错 → 读到别的槽的地址；把 `PD_RX_BUFFER_BASE` 与四槽区地址重叠 → PL 的 DMA 把事件流覆盖掉刚采的快照，或 CPU 读到的归档数据是混在一起的乱码。

---

## 10. 自测 10 问（覆盖全文，先想再看答案在代码哪一行）

1. Zynq 把 PS 和 PL 做进同一颗芯片，主要带来了哪三类片内通道？（提示：GP/HP/ACP，见第 2 节）
2. 本工程 PS 访问 PL 寄存器用的是哪种 AXI 口？PL 把数据写进 DDR 用的是哪种？（`.bd`：`PCW_USE_M_AXI_GP0`、`PCW_USE_S_AXI_HP0/1`）
3. 说出本工程四个 AXI-Lite/AXI 设备的基地址（`pd_feature_bd.bd` → `addressing`）。
4. 为什么"bit 与 ELF 必须同源"？若不匹配，软件哪一行的读写会暴露问题？（`pd_hw_map.h` 的偏移定义 + `pd_acquisition_core.c:41-43`）
5. 裸机里为什么用 `xil_printf` 而不是 `printf`？`volatile` 在 `g_pd_acq` 上的作用是什么？（`pd_acquisition_core.c:27`）
6. PL 经 DMA 往 DDR 写了新数据，CPU 要读之前必须做什么？写出 `pd_acquisition_core.c:145` 那一行的调用名与作用。
7. 启动一次 DMA 之前，`:169-170` 为什么先 `Flush` 再 `Invalidate`？顺序反了会怎样？
8. `xlconcat_0` 把哪三路中断合成一路送 PS？本工程为什么最终用"轮询"而不是中断？（`sw/pd_snapshot_poll.c:4-8`）
9. lwIP RAW 模式下，`pd_tcp_service_poll()` 里三个周期调用分别是什么、各管什么？（`pd_tcp_service.c:767-773`）
10. `tcp_write` 之前为什么要先查 `tcp_sndbuf`？不查会有什么后果？（`pd_tcp_service.c:56`）

---

### 附：本文引用的关键文件清单（便于你回查）
- `pd_feature_bd_2020_2.srcs/sources_1/bd/pd_feature_bd/pd_feature_bd.bd`（`components`、`addressing`、`interface_nets`、`nets`）
- `sw/ps_service/include/pd_hw_map.h`
- `sw/ps_service/src/pd_acquisition_core.c`
- `sw/ps_service/src/main.c`、`sw/ps_service/tcp/main.c`
- `sw/ps_service/tcp/pd_tcp_service.c`
- `sw/pd_snapshot_poll.c`
