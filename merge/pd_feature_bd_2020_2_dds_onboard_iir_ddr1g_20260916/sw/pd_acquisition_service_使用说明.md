# PS 采集软件 V1：`pd_acquisition_service.c`

## 目标和边界

这是当前 DDS 原型的正式 PS 采集骨架，不修改 PL RTL。它在裸机 `ps7_cortexa9_0`
上完成四件事：

1. 接收 `pd_feature_0/m_axis` 的变长 AXI-Stream 包到 PS DDR；
2. 校验每包以 type `0x01` 的周期统计字结束；
3. 收到自动原始快照后，锁定硬件槽、复制完整原始数据到软件归档区、释放槽并恢复采集；
4. 维护事件和快照元数据循环队列，为下一阶段上位机协议提供固定数据源。

它**不会**写滤波器系数或特征阈值。滤波设置仍由 `pd_filter_apply.c` 管理。

## 固定 DDR 归档契约

| 区域 | 地址范围 | 大小 | 用途 |
|---|---:|---:|---|
| PL 原始环 | `0x1000_2000–0x1800_0000` | 约 128 MiB | PL 连续原始采样 |
| PL 硬件槽 | `0x2000_1000–0x2300_1000` | 48 MiB | 四个 12 MiB 自动快照槽 |
| PS 快照归档 | `0x2400_0000–0x2700_0000` | 48 MiB | 保留最近 4 个完整原始快照 |
| PS 事件归档 | `0x2700_0000–0x2710_0000` | 1 MiB | 保留最近 16 个特征 AXIS 包 |

这些区域位于当前 1 GiB PS DDR 映射内，且不与应用加载区、原始环、PL 硬件槽重叠。未来修改
DDR 分区或 linker script 时必须保留这些地址，或同步修改本文件中的宏。

## 元数据

全局符号 `g_acq` 的类型为 `pd_acq_shared_t`，内容包括：

- `event[]`：包序号、PS DDR 归档地址、字节数、peak/cycle 字数；
- `snapshot[]`：归档序号、硬件源槽、源/目的地址、长度、硬件槽序号和 flags；
- `event_overwrites` / `snapshot_overwrites`：循环归档覆盖计数；
- 最后一次 DMA、DDR 和槽状态字。

当前没有上位机协议，因此归档满时采用“覆盖最旧记录并递增计数”。这不是静默丢失：上位机接入前，
串口报告中的 `ev_ovw`、`snap_ovw` 就是数据未及时取走的证据。

## UART 上位机控制协议

当前 XSA 已启用 PS UART0（`0xE000_0000`）。启动 ELF 后服务保持空闲，串口以
115200-8-N-1 发送一行 ASCII 命令并以回车结束：

| 命令 | 含义 |
|---|---|
| `HELP` | 显示命令表 |
| `START 128` | 受限采集 128 个 AXIS 包，自动清理并回到空闲 |
| `START 0` | 连续采集，直到收到 `STOP` |
| `STOP` | 受控停止；DMA 等待期间也会轮询该命令 |
| `STATUS` | 输出运行状态、归档计数、DDR/槽/DMA 状态和硬件拒绝数 |
| `EVENT n` | 输出事件归档槽 `n`（0..15）的元数据 |
| `SNAP n` | 输出快照归档槽 `n`（0..3）的元数据 |
| `CLEAR` | 仅空闲时清除软件元数据计数，不修改 PL 数据区 |
| `QUIT` | 停止（如正在运行）并让 CPU 进入 WFI |

命令传输的是控制与元数据，**不通过 UART 发送 3 MiB 原始快照**。UART 带宽不适合原始
波形搬运；后续以太网协议将使用 `g_acq` 中的地址/长度来分块发送归档内容。

## 首次运行

1. 在 Vitis `app_component2/src` 中只保留一个带 `main()` 的源文件。
2. 将 `pd_acquisition_service.c` 复制为 `main.c`；不要同时保留旧的
   `pd_capture_service.c` 或另一个 `main.c`。
3. Build 后运行，串口显示 `READY`。输入 `START 128`。
4. 通过标志：

   ```text
   ACQ_CLEAN_STOP ddr=... slot=00080000 polls=... drained=<0 或更多>
   ACQ_SERVICE_PASS packets=128 events=128 snapshots=<大于 0> ...
   ```

   `ACQ_CLEAN_STOP` 的 `slot` 低 12 位必须为零：没有 valid、busy 或 locked 槽。
   `bit19/SLOT_READY` 可以仍为 1，它是通知位而不是槽占用。`drained>0` 表示停止请求
   到达 PL 前已有一次事件被接受，服务已在退出前将该末尾快照归档而非丢弃。

5. 初次通过后输入 `START 0` 即进入连续采集，输入 `STOP` 受控结束；无需重新编译。

## 后续开发接口

该版本已经实现 UART 控制和元数据查询，但尚未实现高速网络数据传输。下一阶段以太网接口应：

1. 读取 `g_acq` 的稳定快照；
2. 根据 `event[]` / `snapshot[]` 的地址和长度读取归档数据；
3. 在发送前比较序号，检测循环区已覆盖；
4. 增加 start/stop/status/clear 命令，不直接干预 PL 的 W1P 槽命令。
