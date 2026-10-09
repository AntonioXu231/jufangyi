# 65 MSPS MATLAB 局放激励与验证说明

## 最终采样配置

- 器件：Zynq-7020 `xc7z020clg400-2`，Vivado 2020.2。
- PL 系统时钟：130 MHz；ADC/DDS 采样时钟：65 MHz，整数比 2:1。
- 工频：50 Hz；每周期 1,300,000 点；40 ms 完整四通道仿真记录为 2,600,000 点。
- 每个采样时刻按 `{CH3, CH2, CH1, CH0}` 打包成 48 位，每周期原始数据 7,800,000 字节。

## 生成完整的 MATLAB 仿真波形

运行 [`generate_pd_adc_waveform_physical_model.m`](generate_pd_adc_waveform_physical_model.m)：

```matlab
run('F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916/sim/generate_pd_adc_waveform_physical_model.m')
```

脚本输出：

- `pd_adc_4ch_65m_40ms.mem`：2,600,000 行、每行 12 个十六进制字符的四通道采样数据。
- `pd_event_truth_physical_65m.csv`：每个注入脉冲的通道、周期、样点、相位、极性、幅度和振铃频率。
- `coe_templates/pd_pulse_ch0.coe` 至 `pd_pulse_ch3.coe`：四路各 1024 个有符号 Q1.11 脉冲形状采样，供板载 ROM 流式播放。
- `pd_stream_event_schedule_65m.csv`：重复播放周期的事件时刻；邻近事件如发生模板重叠，会按需顺延并记录实际相位。
- RTL 目录下的 `pd_pd_event_schedule.v`：与上述 CSV 同步生成的事件调度器。

波形含快速前沿、衰减尾波、传感器振铃、50 Hz 相位簇、通道串扰及带限噪声；它是可复现的“物理启发式合成激励”，不是现场采集记录，也不能当作标定/验收实测数据。现场真实波形必须由实际 ADC 采集后导入。

`.mem` 是完整双周期仿真的离线输入，不会被综合到 FPGA，也不是板载 DDS 的数据源。板载测试只使用四个短 COE 模板和事件调度器；因此无需把 40 ms、2,600,000 点记录装入片上 ROM。MATLAB 文件仍会输出 `.mem`，以保留现有全路径仿真的能力。

## 板载 COE 流式 DDS

板载测试模式每个 65 MHz 时钟输出一组 4×12-bit ADC 码，封装总线仍为 `{CH3, CH2, CH1, CH0}`（48 位是四路同时采样的打包宽度，不代表单路 ADC 是 48 位）。每通道使用一个 1024×12-bit、单端口同步 ROM，按样点地址顺序读取模板；模板约 15.75 μs，事件调度器在每个 50 Hz 周期重复 MATLAB 第一个周期的 `[8, 9, 7, 10]` 个事件及其极性/幅值。四个 ROM 的有效初始化内容共 49,152 bit（6 KiB）；它们只保存短脉冲形状，不保存完整工频周期波形。

新生成/修改 COE 后，在 Vivado 2020.2 中打开本工程并在 Tcl Console 执行：

```tcl
source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916/scripts/13_configure_matlab_coe_roms.tcl}
```

脚本会创建/更新四个 Block Memory Generator 8.4 ROM，加载对应 COE，并将 `pd_pd_event_schedule.v` 加入综合源；不会清除或重置综合/实现运行。已有的 `scripts/06_enable_onboard_dds_test_mode.tcl` 也会调用该配置脚本。

完成 IP 配置后，在 PowerShell 执行：

```powershell
& 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\scripts\14_simulate_matlab_coe_stream.ps1'
```

预期输出 `DDS_COE_STREAM_PASS channels=4 bits_per_channel=12 samples_per_cycle=1300000 events=8/9/7/10`。回归覆盖完整一个 20 ms 周期，检查逐通道事件数、ROM 脉冲可见性、连续 `adc_dv` 和 50 Hz `sync_in` 对齐。仿真工作目录默认放在 `F:\xinya\v5\.codex_tmp\pd_coe_stream_xsim`，以避免 Vivado 在系统盘临时目录空间不足。

OOC 综合检查可执行：

```tcl
source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916/scripts/15_check_pd_coe_source_synthesis.tcl}
```

预期输出 `PD_COE_SOURCE_SYNTH_PASS`。它综合四个 BMG ROM 和独立 DDS 源模块，不重置工程 `synth_1`/`impl_1`；这不是整个 Block Design 的综合/实现签核。

该 DDS 是合成测试激励，不是现场实测波形。真实 ADC 运行也应逐样点连续采集，不需要先把 40 ms 数据放进 BRAM；真实硬件的限制转为 ADC/PL→FIFO/DDR/DMA 的持续吞吐、跨时钟域和缓冲区覆盖/丢样，而不是 ROM 容量。65 MSPS×4 路×12 bit 的原始总输入速率为 390 MB/s（未计协议/对齐开销），必须按实际 DMA/DDR 路径和缓存设计验证。

## 仿真入口

- 完整采集/事件路径：`pd_feature_bd_2020_2.srcs/sim_1/new/tb_pd_feature_dds.v`。顶层为 `tb_pd_feature_dds`，使用 130 MHz 系统时钟、65 MHz ADC 时钟和 1,300,000 点同步周期；先运行 20 ms 锁相预热，再送入 40 ms MATLAB 激励。生成 `.mem` 后运行，预期结尾为 `DDS_TEST_PASS events=... channels=1111`。
- 板载 COE DDS 源级测试：`sim/tb_pd_dds_matlab_template.v`。使用四路 1024×12-bit ROM 在 65 MHz 下验证每个通道的事件脉冲和 50 Hz 同步，预期 `DDS_COE_STREAM_PASS ... events=8/9/7/10`。该测试不读取完整 MATLAB `.mem`。
- IIR 流水吞吐测试：`pd_feature_bd_2020_2.srcs/sim_1/new/tb_pd_filter_chain.v`。以 unity 系数检查四通道滤波开启时能每两拍接收一个样点；Vivado Simulator 2020.2 已通过 `TB_PD_FILTER_CHAIN_PASS`。

板载 DDS 是可综合的短模板自检源，不会把 40 ms 完整 `.mem` 放进 Zynq-7020 片上 ROM。COE 模板由 MATLAB 合成脉冲形状生成，并由 50 Hz 调度器重复播放；它不是完整记录回放或现场 ADC 数据。

本次 ROM 仅接在现有板载测试源 `pd_dds_adc_source` 后，未改局放特征核心、DDR 环形缓存、PS 服务接口或 ILA 配置。切换真实 ADC 时，后续只需撤销/删除该测试源及对应测试模式连接，恢复 ADC 引脚/XDC 接入；BMG 测试 ROM 不属于真实采集必需路径。

## 上板前还需完成

65 MHz Clock Wizard 与 130 MHz 系统时钟的 Block Design 已通过 Vivado 2020.2 `validate_bd_design` 并生成 BD/IP 输出；PS 65 MSPS 源码已同步到 Vitis 应用并成功构建 ELF。综合、实现、时序/资源检查、bitstream 下载、XSA 重新导出以及 Vitis 平台更新仍须作为后续硬件验证步骤完成。下载新 ELF 本身不会改变 PL 的时钟或 DDS。
