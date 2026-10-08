# 板载 DDS 改用 MATLAB 局放脉冲模板

## 本次变更

`pd_dds_adc_source.v` 的接口、26 MSPS 数据有效节拍和 50 Hz `sync_in` 不变。四路原有的短脉冲函数已换成四段各 32 个采样点的 12-bit ADC 码模板，每个 20 ms 周期分别在约 45°、135°、225°、315° 重播。模板取自 `pd_adc_4ch_26m_40ms.mem`；该文件由 `generate_pd_adc_waveform.m` **模拟生成**，并非现场实测数据。背景仍是原来的低幅度三角波，因此目前并未逐点重放 MATLAB 文件中的整段背景和所有脉冲。

完整 40 ms、四路、26 MSPS 文件需要 1,040,000 × 48 = 49,920,000 bit。Zynq-7020 的片上 BRAM 容量不足以按 ROM 放入整段文件；本实现的四路模板只有 4 × 32 × 12 = 1,536 bit。若未来要重放完整实测记录，应存储在 PS DDR 中，经 DMA/AXI-Stream 持续喂入 PL，而不是扩大 `reg` 数组或 `$readmemh` 片上 ROM。

## 文件与导入

- 板载可综合源：`pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_dds_adc_source.v`
- 新增源级测试：`sim/tb_pd_dds_matlab_template.v`
- 模板来源：`sim/pd_adc_4ch_26m_40ms.mem`、`sim/generate_pd_adc_waveform.m`

原 Vivado 工程已引用同一路径的 `pd_dds_adc_source.v`，不需要在 BD 中新增 IP 或改端口。可选的源级测试只需将上述 `.v` 与测试文件加入 `sim_1`，把仿真 top 设为 `tb_pd_dds_matlab_template`。预期输出 `DDS_MATLAB_TEMPLATE_PASS channels=4 period_samples=520000`。此测试不读取大 `.mem` 文件；原顶层 MATLAB 测试仍独立使用完整文件。

用户自行完成仿真、综合、实现和 bitstream。只有新的 `.bit` 真正下载到板端，板载 DDS 才会变化。若 Vitis 启动配置自动下载平台内旧的 bitstream，需要从新 Vivado 实现导出带 bitstream 的 XSA 并更新平台，或明确指定新的 `.bit`；单独更新 ELF 不会改变 DDS。

## 板端验收

1. 先确认新 bitstream 已下载，再运行已有 PS 采集服务，并用 TCP `CONFIG` 检查服务协议版本。UI/PS 的相位显示版本应为 `api=13`；`SCOPE PHASE` 预期四路 `wins=1024,1024,1024,1024`，约一个完整 50 Hz 周期后 `lock=f`。
2. 连续波形应有各通道对应的窄脉冲；每 20 ms 重现一次。相位椭圆只在 PL 输出峰值事件且同步锁定时增加真实相位点。当前原始波形帧和 PL 峰值事件**没有共同样本编号**，因此不能宣称两处高亮是同一采样点的严格对齐。
3. 模板的主偏离量约为 CH0 1130、CH1 879、CH2 337、CH3 707 ADC LSB。上位机候选脉冲阈值若仍为 `850 raw ADC`，通常只能看到 CH0/CH1；要观察四路可先试 `250 raw ADC`，再按噪声实测调整。PL 峰值事件是否出现还取决于其独立阈值和滤波配置。
4. 如需真正的现场实测波形，请提供原始 `.mat`/CSV、四路编码方式、采样率、ADC 零点及幅值标定。当前的“真实”仅指模板是从 MATLAB 文件逐点提取，不代表它是现场采集。

## 65 MSPS 全采样仿真激励

`generate_pd_adc_waveform_physical_model.m` 生成 65 MSPS、50 Hz、40 ms 的四通道完整仿真向量：每周期 1,300,000 个采样时刻，共 2,600,000 行 48-bit `{CH3,CH2,CH1,CH0}`，并输出事件真值 CSV。波形是包含快前沿、传感器阻尼振铃、50 Hz 相位簇、通道串扰及带限噪声的**物理启发式合成数据**，不是现场测量。

完整功能仿真 `pd_feature_bd_2020_2.srcs/sim_1/new/tb_pd_feature_dds.v` 已与该 65 MSPS 激励配套：ADC 时钟 65 MHz，系统时钟 130 MHz，`INPUT_CDC=1`、`SAMPLE_HZ=65000000`，同步周期 1,300,000 点；测试仍先运行 20 ms 安静周期，再输入 40 ms MATLAB 数据。运行生成器后，需将生成的 `.mem` 加入 Vivado 仿真工作目录，再运行该 testbench。

注意：这只把**完整功能仿真**切到 65 MSPS。板载自检 DDS `pd_dds_adc_source.v` 仍明确以 26 MHz 运行并输出 32 点短模板；本 MATLAB 文件不会改变其综合实现或板上波形。要让真实板端以 65 MSPS 工作，还必须另外核实并同步修改 ADC/PL 时钟、BD/XDC、DDS/CDC 参数、采集缓冲大小和 PS 采样率配置，不能仅凭仿真通过就上板。

这次未在本机运行 Vivado、Vitis、板端或 Qt；上述均为用户执行时的预期验收条件。
