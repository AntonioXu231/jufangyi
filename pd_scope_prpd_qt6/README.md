# 局放四通道实时波形与相位图谱（Qt 6）

本目录是独立上位机工程。真实 Zynq 模式同时显示四通道整周期波形包络、PL 峰值事件的 360° 相位椭圆，以及逐字事件表；另有单通道页，可选择通道查看原始采样窗口和对应 PS FFT。模拟模式仅用于 UI 验证，不是实测数据。

## 数据来源与边界

- 65 MSPS配置下，`SCOPE ENVELOPE`从PS归档读取完整周期并压缩为1024列min/max，每帧传输16384字节供实时预览。PS扫描时固定当前四槽归档记录；如果新硬件快照正要复用该槽，PS计入`STATUS snap_pin_drop`并让出该新快照，保证当前包络不被覆盖。完整四通道原始点可通过“读取完整SNAP + PS FFT”手动读取；相位列基于工频周期边界，未锁相通道不绘制同步轨迹。
- `SCOPE NEXT` 继续返回四通道 12-bit ADC 原始短窗口，Qt 中心化为约 ±2048 ADC 码，在工作线程接收并 CRC32 校验。此短窗只供单通道页与 PS FFT 使用，不会被误拉伸到 360° 椭圆。`SCOPE ENVELOPE` 的目标刷新间隔为 20 ms；实际帧率受快照产出、PS 归约、TCP 和板端负载限制，若归档槽被覆盖，板端会报告 `SCOPE_ENV SKIP`，客户端保留上一帧并等下一帧。
- `SCOPE PEAKS` 的 `PEAKS V4` 返回一个完整PL事件归档包；其相位窗和锁相掩码取自该包归档时PS保存的元数据。批量V4则在每个二进制记录头单独附带这些字段。Qt按记录元数据解析phase_index，未锁相事件仍进入事件表但不伪称有效相位。
- API 18 对每个事件归档包保存其相位窗与锁相掩码，Qt用该包当时的元数据解相位，避免后续配置变化重解释旧数据；并新增 `FFT FULL SNAP` 完整原始快照读取。`SCOPE PEAKS BATCH 32` 仍传送最多32个完整事件包，CRC覆盖包头、逐包元数据和事件字。
- 事件表按主机接收时刻保留 15 秒，列出 PL 包序号、包内字序号、通道、相位、原始有符号 Q8.8 码与主机阈值结果。椭圆仅将有效相位且通过主机阈值的事件累积为上下刻度，保留约 15 秒；绘图采用 0.1° 相位桶，不截断原始事件表。
- “图谱阈值”是 Qt 对 PL 峰值 Q8.8 原始码的显示筛选，不写 PL 寄存器，也不影响事件归档。“波形阈值”仍是独立的 ADC 码阈值。两者的数值单位不同，均不是 pC 标定值。
- `PS跳过` 表示 PS 归档游标越过保留窗口或遇到坏包；`序号缺口` 表示 Qt 实际收到的事件包序号不连续；`CRC错` 表示收到的数据未通过校验。三者都不是“检测到的局放数”。

全周期椭圆轨迹将每个相位列的 ADC min/max 映射成**上下偏移**，横向位置固定在对应椭圆相位，不再用 ADC 幅值把轨迹向椭圆内外拉伸。每列代表原始周期中一段样本区间；细节少于屏幕像素时按列保留 min/max 绘制，原始点仍可由下述完整 SNAP 读取。

当前波形帧与 PL 事件包没有共同逐样本时间戳，所以只能并排显示，不应宣称椭圆中的某一刻度与波形上的某一候选是同一个物理脉冲。15 秒从主机收到事件时算起，不是硬件发生时刻。即使没有上位机损失，PL 触发/特征模块未报告的信号也无法由 Qt 补回。

## 必须同步的 PS 源码

基准源码位于：

`F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\ps_service`

在当前 Vitis 应用 `F:\ps\lwip_echo_server10\src` 中，同步基准 PS 源：

1. `include/pd_acquisition.h` → 应用 `src/pd_acquisition.h`（2048 个事件归档槽）；
2. `include/pd_spectrum.h` → 应用 `src/pd_spectrum.h`；
3. `src/pd_spectrum.c` → 应用 `src/pd_spectrum.c`；
4. `tcp/pd_tcp_service.c` → 应用 `src/pd_tcp_service.c`（API 18、批量完整事件包、实时PS FFT、归档同窗/完整波形+FFT及全周期包络）；并同步`tcp/modules/`到应用`src/modules/`，这些`.inc`不单独编译。Vitis预检脚本会逐文件核对当前server10源码是否与基准一致；
5. 若应用的硬件映射头不是基准版，再同步 `include/pd_hw_map.h` → 应用 `src/pd_hw_map.h`。

其余 PS 源文件继续使用同一基准目录的一致版本；不要同时导入 `src/main.c` 与 `tcp/main.c`。本次没有修改 PL；必须由你在 Vitis 构建并下载新的 ELF。Qt 不会替你编译或下载。

成功下载后，先用TCP命令`CONFIG`核对`sample_hz=65000000`、`api=18 event_slots=2048 scope_fft=ps_q15_1024x1_bins513 snap_fft_wave=ps_q15_1024x1_samples_plus_bins snap_full=raw6b_1300000 event_meta=per_packet_v1 scope_env=cycle1300000_bins1024_ch4_minmax16 scope_peaks_batch=32`。板端必须更新到支持完整SNAP与逐包相位元数据的PS ELF。当前app名称为`lwip_echo_server10`，平台为`platform5`。

## Qt Creator 启动

在 Qt Creator 中打开 `F:\xinya\v5\pd_scope_prpd_qt6\CMakeLists.txt`，选择 Qt 6 Desktop Kit，由你自行配置、构建和运行。连接 `192.168.1.10:6001` 后默认勾选“事件优先”：全周期包络以 20 ms 为目标轮询周期，事件通过批量接口追赶。新增常驻流诊断行，分别显示原始短帧、完整周期包络、实时/归档 PS FFT、包络 NONE/SKIP、事件序号缺口、PS 跳过与 CRC 错；计数为零/保持不变时不能声称数据链路正常。单通道页可读 SNAP CATALOG 并选择序号、通道和起点执行匹配的归档波形+PS FFT。

主视图分为“四通道实时图谱”和“单通道波形 + FFT”两个页面。新页实时预览显示同一 1024 点短帧（65 MSPS下约15.75 μs）及其PS端FFT。选择“PS FFT 归档快照”可快速读取所选窗口；选择“读取完整 SNAP + PS FFT”会短暂停止采集，分块下载所选SNAP的全部1,300,000点，并由PS对选定起点的同一1024点窗口计算FFT。完整四通道原始数据保存在Qt工作线程内存中，单通道波形按屏幕列绘制每列min/max，因此尖峰可见且每个原始点仍参与绘制归约。下载完成后采集自动恢复；归档页面显示SNAP、通道、总点数和FFT窗口起点。Qt不执行FFT，只校验PS数据和CRC并绘图。1024点FFT频率间隔约63.48 kHz。

## 板端验收建议

1. 板端独立TCP会话发送`CONFIG`，核对`sample_hz=65000000 api=18`以及`scope_fft`、`snap_fft_wave`、`snap_full`、`scope_env`、`scope_peaks_batch`字段。断开该会话后再让Qt连接，避免单客户端端口冲突。
2. Qt 连接后自动发 `START 0`、`SCOPE ON 1024`，请求 `SCOPE ENVELOPE`、`SCOPE PEAKS`，并按需读取 `SCOPE NEXT` 与所选通道的 `SCOPE FFT CHANNEL n`（n 为 0..3）。四通道波形应按完整周期刷新；PS FFT 的波形/频谱序号仍需匹配。若未锁相，Qt 不应把快照伪标为 PL 同步相位。
3. 如需独立核对协议，可在**Qt断开时**手工发送`SCOPE PEAKS`：非空响应为`PEAKS V4`头后紧跟`bytes`个事件字节。客户端还需用头中的归档时锁相位窗解析相位；不能用逐行`ReadLine()`消费二进制负载。
4. 在产生高密度事件的 DDS 测试下，观察 15 秒事件表是否持续增加并到期删除，椭圆上下刻度是否累积；检查 `PS跳过/序号缺口/CRC错`。若任何计数增长，不能声称零损失，应记录负载、波形帧率和丢失量。
5. 调节“图谱阈值”只应改变通过标记和椭圆刻度，不应改变事件表的原始行数，也不应改写 PL 门限。单通道和整体鼠标滚轮缩放、复位应继续有效。

2026-10-08的API17板上截图曾显示PS跳过/事件缺口达到913并持续增加，因此当时事件链路未通过无损验收。当前Qt在积压达到256个事件包时持续排空完整批次，待积压回落后再请求包络；待取包数与完整SNAP读取进度也会显示。API18及绘图修改尚待用户构建和板上验收；相同DDS负载连续观察至少60秒，确认gap/skipped/CRC/SKIP增量为0且backlog有界后，才通过传输验收。

“完整周期包络”计数是 TCP 应答次数；同一 `snap_seq` 的缓存应答不代表新增快照。当前ROM调度源的位置以 `sim/pd_event_truth_physical_65m.csv` 与 `sim/coe_templates/pd_stream_event_schedule_65m.csv` 为准；后者由MATLAB脚本生成。归档 FFT 选择 `START 0` 时只观察周期起点的1024点窗口，通常看不到毫秒级相位位置的脉冲。可选择“读取完整 SNAP + PS FFT”先确认整周期，再将FFT起点定位到感兴趣的事件区域。PL事件包与SNAP目前仍没有共同的逐样本事件时间戳，完整波形与事件刻度不能据此声称为同一次物理脉冲。
