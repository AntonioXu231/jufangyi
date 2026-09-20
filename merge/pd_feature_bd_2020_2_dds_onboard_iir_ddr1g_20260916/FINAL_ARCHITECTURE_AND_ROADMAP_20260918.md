# Final Architecture And Implementation Roadmap

Date: 2026-09-18  
Project: `pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`  
Device: `xc7z020clg400-2`  
Vivado: `2020.2`  
Prototype sample rate: `26 MSPS`  
PL clock: `130 MHz`

## 1. Final Decision

The project uses one acquisition path with independent storage, real-time decision, and host-side analysis responsibilities:

```text
ADC/DDS raw samples
  -> raw DDR ring and snapshot storage
  -> PL real-time algorithm path
       -> pd_filter_0 preprocessing
       -> pd_feature_0 algorithm core
            -> peak/threshold/event packet
            -> real-time PL PRPD
            -> event_accept snapshot trigger
  -> PS snapshot analysis
       -> raw sample unpacking
       -> FFT/spectrum/selection
       -> offline PRPD cross-check and reporting
```

The final architectural rules are:

1. `pd_feature_0` is the PL real-time feature extraction and event-decision core.
2. `pd_filter_0` is an optional configurable preprocessing stage for `pd_feature_0`.
3. PL PRPD remains the product real-time PRPD path.
4. PS PRPD is only for offline cross-check, display, and algorithm comparison.
5. FFT belongs to the `pd_feature` algorithm domain, but executes on PS using raw DDR snapshots.
6. The four-lane PL FFT is not restored.
7. The analysis path must never apply backpressure to acquisition, filtering, feature extraction, DMA, or snapshot storage.
8. The current five-cycle IIR must not be driven directly at 65 MSPS.

## 2. Current Dataflow

```text
pd_dds_0 / real ADC
        | raw adc_data/adc_dv
        v
pd_ddr_0: CDC, raw ring write, freeze, four-slot snapshot
        |
        +-- raw samples -> DDR ring -> snapshot slot -> PS
        |
        +-- o_feat_data/o_feat_dv
                v
            pd_filter_0
                v
            pd_feature_0
                +-- real-time feature extraction
                +-- threshold and polarity decision
                +-- PL PRPD accumulation
                +-- m_axis event packet
                +-- o_event_accept
                         v
                    pd_snapshot_trigger
                         v
                    freeze_trig
```

The current BD connections are:

```text
pd_ddr_0/o_feat_data  -> pd_filter_0/adc_data
pd_ddr_0/o_feat_dv    -> pd_filter_0/adc_dv
pd_filter_0/filt_data -> pd_feature_0/adc_data
pd_filter_0/filt_dv   -> pd_feature_0/adc_dv
pd_feature_0/o_event_accept -> pd_ddr_0/i_event_accept
```

DDR stores raw samples. The filter is only on the real-time decision path.

## 3. Responsibility Boundaries

### PL algorithm core: `pd_feature_0`

```text
phase window and cycle synchronization
baseline/phase processing
positive and negative peak extraction
threshold decision
deadtime and event de-duplication
event sequence
event packet encoding
PL PRPD accumulation
event FIFO
event_accept
feature status and IRQ
```

### Filter preprocessing: `pd_filter_0`

```text
shadow coefficient writes
coefficient readback
APPLY
global/channel bypass
real-time band shaping before feature decision
```

### PS analysis domain

```text
snapshot descriptor and slot management
raw 48-bit sample unpacking
offset-binary conversion
window and DC removal
FFT
spectrum peak and frequency selection
offline PRPD cross-check
statistics, storage, and communication
```

## 4. Evidence Status

The following are supported by current evidence:

```text
TB_PD_SNAPSHOT_TRIGGER_PASS
TB_PD_FILTER_CHAIN_PASS
bitstream/XSA generated
pd_ddr_0/irq connected to IRQ_F2P through xlconcat
FILTER_APPLY_VERIFY_PASS reported by the user
SNAPSHOT_POLL_PASS reported by the user
slot descriptor and DDR first/last sample read successfully
```

The following must not yet be declared complete:

```text
feature event output under a valid DMA TREADY condition
full top-level snapshot simulation
pd_ddr IRQ GIC service
filter-enabled feature threshold calibration
PS FFT/spectrum service
four-slot sustained operation
real AD9226 input
host protocol and UI
```

`SNAPSHOT_POLL_PASS` proves the trigger, freeze, copy, slot, and PS read path. It does not by itself prove the complete `pd_feature_0` event stream.

## 5. DDR_STATUS Error Gate

The `DDR_STATUS` bits are:

```text
bit[0] = acq_en
bit[1] = copy_busy
bit[2] = hiwm
bit[3] = freeze_done
bit[4] = snap_overrun
bit[5] = err
bit[6] = copy_done_sticky
bit[7] = copy_pending
bit[8] = copy_err
bit[9] = snapshot_cfg_err
```

Therefore:

```text
DDR_STATUS=0x00000021
```

means `acq_en=1` and `err=1`; it is not a clean-pass value. The PS test must fail or at least report any of these bits:

```c
DDR_STATUS bit[5] err
DDR_STATUS bit[8] copy_err
DDR_STATUS bit[9] snapshot_cfg_err
SLOT_STATUS bit[13] cfg_err
SLOT_STATUS bit[14] req_overflow
SLOT_STATUS bit[15] cmd_err
```

The source of any sticky `err` must be explained before the prototype is called clean.

## 6. Address, Format, and Alignment Contract

```text
pd_feature_0 = 0x40000000
pd_ddr_0     = 0x40010000
pd_filter_0  = 0x40020000
axi_dma_0    = 0x40400000
```

One sample time is 48 bits:

```text
[47:36] ch3
[35:24] ch2
[23:12] ch1
[11:0]  ch0
```

Four sample times form one 24-byte raw block.

Alignment scope:

```text
4 KiB: partition bases, region boundaries, and DataMover regions
24 B: one snapshot data length and raw block boundaries
8 B: DataMover address/access requirement; implied by 24-byte alignment
```

A single snapshot length does not need to be 4 KiB or 0x3000 aligned. `3120000` bytes is legal because it is divisible by 24.

## 7. Filter-to-Feature Calibration Gate

When the filter leaves bypass, the feature code-domain parameters must be calibrated as one set:

```text
cfg_thresh
cfg_pos_thr[6]
cfg_neg_thr[6]
cfg_deadtime
cfg_scale
cfg_reg0_q/cfg_reg1_q
```

Required A/B procedure:

```text
1. Run the same excitation in bypass mode.
2. Record event rate, positive/negative counts, amplitude bins, and PL PRPD.
3. Apply the filter and repeat exactly the same excitation.
4. Sweep threshold, polarity thresholds, deadtime, and scale together.
5. Check whether filter ringing creates an extra opposite-polarity event.
6. Save the selected calibration values with a version.
```

It is forbidden to calibrate in bypass and run in filter-enabled mode without revalidation.

## 8. DMA/TREADY Gate

The current `pd_feature_0/m_axis` is connected to:

```text
axi_dma_0/S_AXIS_S2MM
ila_0/SLOT_0_AXIS
```

When DMA is not started, `m_axis_tready` must not be assumed to be high. Before declaring the PL algorithm core board-verified, do one of the following:

1. Start DMA S2MM and continuously consume the event stream; or
2. Add a permanently ready consumer and revalidate the interface and resource cost.

The automatic snapshot event pulse is not sufficient evidence for the downstream AXI event stream, because `o_event_accept` is generated at the local event FIFO acceptance point.

## 9. 65 MSPS Decision

The current `pd_iir_biquad` needs five clocks per sample. At 130 MHz it cannot accept a direct 65 MSPS stream.

The preferred future architecture is:

```text
65 MSPS raw acquisition and DDR archive
 -> CIC 4:1 on the decision path
 -> 16.25 MSPS
 -> IIR filter
 -> pd_feature decision core
```

Do not implement the PS-side FFT and long-term software assumptions as if 65 MSPS were already fixed. First lock the CIC/CDC/clock/parameter architecture.

## 10. Final Work Stages

### Stage 0: Freeze contracts

```text
DDR stores raw ADC data
PL PRPD is the product real-time path
PS FFT is the analysis path
65 MSPS architecture is explicitly selected or marked unsupported
interface contract is corrected from filtered DDR data to raw DDR data
analog front-end high-pass/band-pass status is recorded
```

### Stage 1: Formal PS driver

Create:

```text
pd_hw_regs.h
pd_filter.c/h
pd_snapshot.c/h
pd_dma.c/h
pd_feature.c/h
pd_device.c/h
```

Use parameterized APIs:

```c
pd_filter_apply_bandpass(uint32_t fs_hz, const pd_band_t *band);
pd_feature_recalibrate(const pd_feature_cal_t *cal);
```

### Stage 2: Top-level RTL simulation

Cover:

```text
event accept
automatic freeze
cycle boundary
DataMover copy completion
snapshot_ready/IRQ
slot lock/release
freeze_resume
slot0..slot3 rotation
full-slot rejection and counters
```

### Stage 3: DMA/TREADY and feature gate

```text
start DMA S2MM
confirm m_axis_tready
check event packet, TLAST, sequence, and DMA errors
verify threshold APPLY takes effect
```

### Stage 4: Filter/feature A/B calibration

```text
bypass capture
filter-enabled capture
joint threshold/polarity/deadtime/scale calibration
save calibration data and version
```

### Stage 5: PS analysis domain

```text
snapshot unpack
offset-binary to signed conversion: signed = raw - 2048
window/DC removal
four-channel FFT
spectrum peak and selection
offline PRPD cross-check
```

### Stage 6: IRQ and sustained rotation

```text
GIC ISR
snapshot_irq_pending
slot lock/read/release
freeze_resume
measure one-slot processing time < 80 ms
run slot0 -> slot1 -> slot2 -> slot3 continuously
```

### Stage 7: 65 MSPS architecture

Only after Stage 0:

```text
CIC 4:1
CDC/FIFO depth
16.25 MSPS IIR
phase-window and sample-rate registers
threshold recalibration
bandwidth and DDR contention verification
```

### Stage 8: Communication and host

```text
TCP configuration/status/snapshot access
UDP event and spectrum summaries
waveform, spectrum, PRPD, alarms, and logs
```

### Stage 9: Real AD9226

```text
interface mode
sample clock
I/O voltage and pins
input timing
CDC
XDC
board-level sampling validation
```

## 11. Mandatory Gates

### B0: BD/IP generation gate

After any BD/IP change and before synthesis:

```text
module_ref XML contains dataType="integer"
generated Verilog parameters have no quoted numeric values
BD netlist [0:0] count is approximately 12
.xpr has no cross-tree $PPRDIR/../ references
```

Any failure stops the flow.

### Simulation gate

```text
snapshot trigger module simulation passes
filter-chain module simulation passes
top-level snapshot simulation passes
DMA/TREADY event-stream regression passes
```

### Release timing gate

```text
Place 30-487 = 0
WNS >= 0
TNS = 0
WHS >= 0
THS = 0
no blocking DRC errors
```

The current known implementation result was `WNS=-0.054 ns`, `TNS=-0.491 ns`; this is a debug bitstream, not a release bitstream.

## 12. Completion Criteria

### Prototype hardware path

```text
real serial FILTER_APPLY_VERIFY_PASS log
no unexplained DDR_STATUS[5] err
SNAPSHOT_POLL_PASS
```

### PL algorithm core

```text
DMA/TREADY event-stream validation
threshold APPLY validation
filter A/B calibration
PL PRPD cross-check
```

### Software analysis system

```text
formal PS driver
IRQ snapshot service
snapshot unpack
PS FFT/spectrum/selection
four-slot sustained rotation
error recovery
```

### Product release

```text
real AD9226
locked 65 MSPS architecture or an explicit product limitation
host protocol
long-duration operation
final timing closure
matching bit/XSA/Vitis versions
```

## 13. Immediate Next Actions

```text
1. Add DDR_STATUS[5] to pd_snapshot_poll.c failure handling.
2. Preserve the real serial FILTER_APPLY_VERIFY_PASS log.
3. Start DMA S2MM and complete the pd_feature m_axis/TREADY gate.
4. Add and run top-level snapshot simulation.
5. Run bypass/filter-enabled feature A/B calibration.
6. Split the formal PS driver and PS FFT analysis service.
7. Lock the 65 MSPS CIC/decimation architecture.
8. Proceed to real AD9226, communication, and productization.
```

This plan preserves the verified dataflow: raw samples go to DDR, `pd_filter_0 -> pd_feature_0` performs real-time decision processing, PL retains the real-time PRPD path, and PS performs FFT analysis on raw snapshots.
