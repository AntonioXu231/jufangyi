# `pd_capture_service` 配置与验收

## 目的

`pd_capture_service.c` 是当前 DDS 原型的首个 PS 侧集成服务。一次受限运行同时验证：

```text
pd_feature_0/m_axis -> AXI DMA S2MM -> HP0 -> PS DDR
event_accept -> 自动冻结 -> 原始数据快照槽 -> PS 锁定/读取/释放
```

它不写滤波系数、不写特征阈值。因此，未标定的算法参数不会在本程序中被意外覆盖。

## 前置硬件条件

1. 使用包含 `pd_snapshot_trigger.v` 和 BD 连接
   `pd_feature_0/o_event_accept -> pd_ddr_0/i_event_accept` 的匹配 bitstream。
2. BD 已生成 output products、wrapper，且 Vivado 已通过 `Validate Design`。
3. Vitis platform 必须由这份匹配 XSA 创建或更新。若 `xparameters.h` 不含
   `XPAR_PD_DDR_0_BASEADDR` 或 AXI DMA 宏，不要手改宏，应重新导出 XSA 并更新 platform。
4. DDS/特征事件源可持续产生事件。滤波器是否已正式下发系数不影响本服务的编译；
   但若要验证“滤波后特征”，应先单独运行 `pd_filter_apply.c` 并确认其通过。

## Vitis 2024.1 操作

1. 在现有 platform 下新建 **Empty Application**（Standalone、`ps7_cortexa9_0`）。
2. 删除模板生成的 `src/main.c`，将
   `sw/pd_capture_service.c` 加入工程，或把它复制为 `src/main.c`。
3. Build。编译错误提示没有 `PD_DDR` 或 `AXIDMA` 宏时，更新 platform/XSA；不要在源文件中
   填写猜测的基地址。
4. 先在 Vivado Hardware Manager 写入与 XSA 完全匹配的 `.bit` / `.ltx`。
5. 在 Vitis 的 launch configuration 中选择：
   - `Target Setup Mode`: **Standalone Debug**；
   - `Board Initialization`: 使用该 XSA 生成的 `ps7_init.tcl`；
   - 勾选 `Run ps7_init` 与 `Run Ps7 Post Init`；
   - 如 bitstream 已在 Vivado 写入，取消 `Program Device`，防止写入不匹配的旧 bitstream；
   - Application 指向本程序生成的 ELF；取消 `Stop at entry`。
6. Launch 后观察串口。运行结束会停在 WFI；下一次下载 ELF 前在 Vitis 终止调试会话即可。

## 通过判据

最终应看到：

```text
CAPTURE_SERVICE_PASS packets=128 snapshots=<大于 0> peak_words=<...>
cycle_words=<至少 128> bytes_avg=<非 0>
```

同时，途中至少有一行：

```text
SNAP 0 slot=<0..3> base=2000xxxx len=<8 字节对齐且非零> ...
```

这说明 PS 已按“锁槽 -> cache invalidate -> 读取 -> 释放 -> FREEZE_RESUME”完成一轮槽生命周期。

在 `PASS` 前还必须出现：

```text
CLEAN_STOP ddr_status=00000000 slot_status=<无 busy 位> polls=<...>
```

此行证明除 S2MM 以外，原始 DDR 环写和独立快照复制也已退出。`SLOT_READY` 可以保留，
它只表示仍有有效描述符，并不表示有拷贝正在进行。

## 失败处理边界

- `CAPTURE_SERVICE_FAIL: automatic snapshot trigger did not arm`：检查事件触发 BD 连接、新 wrapper 和匹配 bitstream。
- `S2MM completion contract failed`：先回退运行 `pd_feature_dma_s2mm_smoke.c`，确认 DMA/HP0/DDR 基础通路。
- `slot manager reported an error`：先运行 `pd_snapshot_poll.c`，确认槽配置、DDR 地址和自动快照基础通路。
- `snapshots=0` 而 DMA 有数据：说明收到了 AXIS 事件包，但未见自动快照完成；重点检查
  `event_accept` 连线和 `SNAP_TRIG_CTRL` 的 enable/mask。

本程序是轮询式、受限的集成服务，不是最终常驻应用。通过后，下一阶段才将轮询替换为
IRQ_F2P 驱动的环形 DMA/快照队列，并接入正式的上位机协议。
