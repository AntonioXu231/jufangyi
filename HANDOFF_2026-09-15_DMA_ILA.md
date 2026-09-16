# PD Feature / DDS / AXI DMA / ILA hardware bring-up handoff

## Purpose of the next session

Continue from the now-stable variable-length S2MM receive implementation, then
perform any later algorithm-value correlation without regressing the verified
AXI-Stream/DMA packet contract.

## Post-fix status (2026-09-15)

The fixed PS probe (`sw/dma_s2mm_probe.c`) has now run continuously beyond
72,000 packets. Representative live output:

```text
completed=72321 DMASR=00001002
packets=72321 min=8 max=8224(max@178) avg=1890
hist(... over=0 zero=0)
```

This proves the repaired receive flow is repeatedly reaching `IOC_Irq + Idle`
without DMA error bits. ILA captures repeatedly show both `type=0x00/TLAST=0`
and `type=0x01/TLAST=1` under `TREADY==1` triggering. The fixed source uses a
65528-byte maximum BTT, non-cacheable DDR buffer and the DMA S2MM_LENGTH
readback to record each TLAST-delimited packet's actual byte count.

## Hardware and tools

- Board: 正点原子领航者 Zynq-7020, `xc7z020clg400-2`.
- Vivado: 2020.2 (`F:\vivado20`).
- Vitis Unified: 2024.1.
- Working Vivado snapshot:
  `F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_20260914\pd_feature_bd_2020_2.xpr`.
- Vitis workspace: `F:\ps`.
- Application: `F:\ps\pd_dma_s2mm_test`.

## Current topology and verified configuration

- On-board DDS drives the four-channel ADC-side test input at 26 MHz.
- `clk_wiz_0/clk_out1` is 130 MHz and clocks feature processing, AXI DMA and
  AXIS ILA. It is derived from PS FCLK, so `ps7_init` must be run before the
  ILA debug hub is expected to appear.
- AXI DMA is Simple mode, S2MM only, 64-bit stream, no SG:
  - AXI Lite: `0x40400000`
  - S2MM stream source: `pd_feature_0/m_axis`
  - S2MM memory path: HP0 to PS DDR
  - DDR test buffer: `0x01100000`
- AXIS ILA (`ila_0`) monitors the same stream. Trigger used:
  `pd_feature_bd_i/pd_feature_0_m_axis_TREADY == 1`, depth 1024, trigger
  position 512.
- Bitstream and matching probes file:
  `F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_20260914\pd_feature_bd_2020_2.runs\impl_1\pd_feature_bd_wrapper.bit`
  and `.ltx` in the same directory.

## Proven hardware evidence

One one-shot 128-byte S2MM test produced:

```text
S2MM before arm: DMACR=00010002 DMASR=00000001
S2MM after arm:  DMACR=00010003 DMASR=00000000 LENGTH=8
S2MM final:      DMACR=00010003 DMASR=00001002 polls=0
```

`DMASR=0x00001002` means `IOC_Irq` plus `Idle`, with no S2MM error bits.
The ILA transitioned to `Idle` after the `TREADY==1` trigger. This is evidence
that a real PL AXIS-to-DMA-to-DDR transaction occurred.

PG021 v7.1 (S2MM_LENGTH, offset 58h) states that at the completion of the S2MM
transfer the register **is** updated with the number of actual bytes written on
the S2MM AXI4 interface. The observed readback of `8` is therefore consistent
with a one-beat (8-byte) TLAST-terminated packet — a cycle-stats packet that
arrived with no preceding peak event. Treat the readback as the byte count, but
confirm it once with a directed test of known packet length before relying on it.

## Historical failure (resolved by the current probe)

To make capture repeatable, the test application was changed to resubmit a
128-byte S2MM Simple transfer every 1 ms. It now reliably fails after a number
of transactions (one observed case was transfer 106):

```text
FAIL: transfer=106 DMACR=00010002 DMASR=00005011 LENGTH=128 polls=20000000
```

Decode of `DMASR=0x00005011`:

- `0x00000001`: Halted
- `0x00000010`: S2MM DMA internal error
- `0x00004000`: error interrupt
- `0x00001000`: IOC bit is also latched

This was a real S2MM failure, not a software timeout to suppress. It is retained
as historical evidence for why the fixed-BTT loop must not be restored.

The contemporaneous ILA capture shows `type=01` and `TLAST=1` at the trigger
point while the stream source is valid. `pd_feature_core.v` defines type `0x01`
as a cycle statistics packet and asserts `TLAST` for that frame tail.

## Root cause and resolution

The stream uses variable-length logical frames: peak events (`type=0x00`) may
precede a cycle statistics packet (`type=0x01`, `TLAST=1`). The PS test uses an
arbitrary fixed 128-byte Simple-DMA BTT. Consequently the S2MM BTT boundary can
be misaligned with AXIS `TLAST`, which is a likely reason for the intermittent
S2MM internal error and for seeing only the first received word parse as a
known event type. This is a hypothesis, not yet a signed-off root cause.

The fixed probe treats S2MM_LENGTH readback as the actual byte count of the
TLAST-delimited packet, arms the channel with the maximum legal BTT (65528 B),
and immediately re-arms after each clean completion. Current data proves the
observed packet maximum is 8224 B, below that BTT limit.

## Current software state

Both files were deliberately kept identical:

- Active Vitis source:
  `F:\ps\pd_dma_s2mm_test\src\dma_s2mm_dds_test.c`
- Snapshot copy:
  `F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_20260914\sw\dma_s2mm_dds_test.c`

The source currently:

- prints DMA CR/SR safely using 32-bit `xil_printf` formats;
- checks S2MM error/halted/idle state;
- decodes the documented 64-bit event formats;
- contains an older fixed-128-byte experiment for historical comparison;
- contains the active `dma_s2mm_probe.c` variable-packet implementation.

## RTL event contract relevant to debugging

See `pd_feature_core.v` around the event pack definitions (approximately lines
544 onward) and `pd_axis_arb.v`.

Peak event (`type=0x00`):

```text
[63:56] type
[55:40] signed q, Q8.8
[39:30] phase
[29]    polarity
[28:27] channel ID
[26:0]  per-channel event sequence
```

Cycle packet (`type=0x01`, `TLAST=1`):

```text
[63:56] type
[55:32] cycle index
[31:30] channel ID
[29:16] cycle event count
[15:0]  cycle qmax
```

`pd_axis_arb` performs beat-level round-robin arbitration. Consequently, frame
semantics and `TLAST` semantics must be reviewed carefully before assigning a
fixed S2MM receive size.

## PG021 verification result (2026-09-15) — contract violation, not an intermittent fault

Verified against PG021 v7.1, the driver shipped with the installed BSP
(`F:\ps\platform\ps7_cortexa9_0\standalone_ps7_cortexa9_0\bsp\libsrc\axidma`,
`axidma_v9_18`), and the IP configuration actually built in this project
(`pd_feature_bd_axi_dma_0_0.xci`).

### What PG021 actually says

- **S2MM_DMASR bit 4 `DMAIntErr`**: "When Scatter Gather is disabled, this error
  is flagged if any error occurs during Memory write **or if the incoming packet
  is bigger than what is specified in the DMA length register**." The programmed
  `S2MM_LENGTH`/BTT is a **buffer bound, not an exact byte count to match**.
- **S2MM_LENGTH**: "This value must be **greater than or equal to the largest
  expected packet** to be received on S2MM AXI4-Stream. Values smaller than the
  received packet result in undefined behavior."
- Simple-mode S2MM terminates the transfer on **TLAST**. If the packet ends
  before BTT is exhausted, the transfer completes with the smaller actual length.

### IP configuration read from the project

| Parameter | Value | Consequence |
|---|---|---|
| `c_include_sg` | 0 | Simple / direct-register mode (as assumed) |
| `c_s_axis_s2mm_tdata_width` | 64 | 8-byte beats; BTT must be a multiple of 8 |
| `c_sg_length_width` | 16 | `MaxTransferLen = 2^16-1 = 65535` bytes; a larger BTT is rejected by `XAxiDma_SimpleTransfer()` with `XST_INVALID_PARAM` |
| `c_s2mm_burst_size` | 16 | — |

### Why a fixed 128-byte BTT is not viable on this design

`pd_feature_core.v` asserts `ev_tlast` only inside `S_CYC` (cycle statistics
packet, ~line 844). `pd_axis_arb.v` forwards `m_tlast = s_tlast[out_ch]` under
beat-level round-robin. One TLAST-delimited S2MM packet is therefore *every beat
since the previous cycle-stats packet, aggregated across all four channels*.
Its length is data-dependent and has **no architectural upper bound**. Programming
a fixed 128-byte BTT is a guaranteed contract violation, not a design margin issue.

### Failure decode, now fully self-consistent

`DMASR = 0x00005011` = `Halted(0x1)` + `DMAIntErr(0x10)` + `IOC_Irq(0x1000)` +
`Err_Irq(0x4000)`; `DMACR = 0x00010002` shows `RS` (bit 0) already cleared — the
documented "halt gracefully" signature of `DMAIntErr`.

The `IOC_Irq` bit must **not** be read as a good transfer. The DMA filled all 16
beats of the 128-byte buffer without seeing TLAST, latched `IOC_Irq` as "buffer
done", then flagged `DMAIntErr` because the packet continued past the programmed
length and halted. That is exactly why IOC and DMAIntErr are set together.

### Second finding: the software cannot detect this error promptly

`XAxiDma_Busy()` in `axidma_v9_18` is only
`((SR & XAXIDMA_IDLE_MASK) ? FALSE : TRUE)`. It does not test `Halted`. After the
graceful halt `Idle = 0`, so `XAxiDma_Busy()` returns TRUE forever and the poll
loop always runs to `POLL_LIMIT` (hence `polls=20000000`). The error is reported
late and the poll count carries no timing information. Poll on
`Idle || error bits || Halted` and sample `DMASR` on every tick.

Separately, `pd_feature_top.v` leaves the per-channel event FIFO `.overflow()`
output unconnected (`.overflow ()`), so event loss during TREADY-low gaps is
invisible to software. `FIFO_AW = 8` means 256 beats per channel.



1. Preserve the original 128-byte failure evidence; do not revert to arbitrary
   fixed BTT transfers.
2. For algorithm validation, inject a known DDS event per channel and compare
   decoded DDR fields (`type`, `ch_id`, `phase`, `q`, sequence) with simulation
   or MATLAB expectations.
3. If changing RTL, ILA probes or BD, re-run implementation and timing checks
   before programming hardware.

## User workflow preferences

- Explain actions one step at a time in Chinese; user is operating Vivado/Vitis
  GUI personally.
- Do not push to GitHub unless the user explicitly says “推送到 GitHub” in the
  current conversation.
- If a GitHub push is requested, first read and follow:
  `F:\xinya\v5\版本管理\README.md`,
  `F:\xinya\v5\版本管理\CHANGELOG.md`, and
  `F:\xinya\v5\版本管理\变更记录\模板.md`.
- Do not overwrite prior version snapshots; create a new directory for each
  versioned push.

## Suggested skills

- `xilinx-suite` — Vivado/Vitis and Zynq workflow.
- `ila-hw-debug` — obtain fresh hardware evidence around the failure.
- `sim-bringup` — create a directed variable-frame / TLAST-versus-BTT simulation.
- `cdc-audit` — only if a later inspection identifies a true crossing concern.

## Decisive next experiment (2026-09-15, no RTL change required)

Single-variable test that discriminates "packet larger than BTT" from "memory-write
error". It needs **no PL rebuild** — only the PS application changes.

1. Keep the failure evidence in the log; do not add reset-and-retry.
2. Set the S2MM BTT to the maximum this build allows, `65528` bytes (multiple of 8,
   below the 65535 `MaxTransferLen` cap), and **remove the 1 ms gap**: re-arm
   immediately after each completion so TREADY is deasserted only for the
   re-arm time (microseconds), not 1 ms.
3. After each completion read `S2MM_LENGTH` and record it as the actual packet
   length. Accumulate min / max / a coarse histogram.
4. Fix the poll: break on `Idle || error bits || Halted`, and log the poll index
   at break so the failure time is measurable.
5. Expected outcomes:
   - Clean for tens of thousands of packets, no error bits, and a finite observed
     max packet length → confirms the mechanism and yields the real packet-length
     distribution. Option A (large BTT) becomes viable if max packet << 65528.
   - `DMAIntErr` still reproduces → the cause is a memory-write error, not packet
     length, and the investigation moves to the HP0 / DDR path.
   - Packet length observed > 65528 → the PL needs fixed-length framing
     (Option B) and the AXI-Stream receive contract must be re-issued.

Follow-up decisions that this experiment unblocks:

- **Option A** — PS arms one large-BTT TLAST-terminated transfer and re-arms
  continuously. Cheapest, no RTL change, but the design still has no upper bound
  on packet length and no protection against a heavy-discharge burst.
- **Option B (recommended for production)** — insert a PL fixed-length framing
  adapter after `pd_axis_arb`: pack beats into fixed-size DMA packets, assert a
  local TLAST at the fixed boundary, pad the tail with a reserved event type, and
  prepend a header word carrying the beat count / flags. This bounds the packet
  length by construction, keeps the event payload format unchanged, and must be
  recorded as a change to the v3.0 interface contract.
- **Option C** — enable the SG engine and use multi-descriptor / cyclic receive.
  Largest change (IP regeneration, XSA and platform rebuild), only worth it if
  sustained rate or lossless capture becomes a requirement.

## Log re-analysis (2026-09-15, second pass)

The 16-word dump from the most recent run is **not** sixteen received words.

```text
00: raw=01000000_C00102DC CYCLE idx=0 ch=3 n=1 qmax=732
01: raw=00FD2C18_02000024 PEAK ...
04: raw=00FD2C18_02000024 PEAK ...   <-- identical to word 01
07: raw=00002A04_DC00004D PEAK ...
08: raw=00002A04_DC00004D PEAK ...   <-- identical to word 07
```

- Word 00 is a **cycle packet** (`type=0x01`, `cycle_idx=0`). Per the RTL, `ev_tlast`
  is asserted only with the cycle packet, so this beat carried TLAST and terminated
  the transfer after **8 bytes**.
- Words 01..15 are therefore **stale DDR content**, not received data. The exact
  duplication at `04 == 01` and `08 == 07` is the give-away: a live event stream
  carries a strictly increasing `seq` and cannot repeat a whole word.
- The success path never printed `S2MM_LENGTH`, so that run still yields no byte
  count. The earlier one-shot test did read back `8`, which is consistent.

Consequence: **there is still no evidence that any multi-beat packet has ever been
received cleanly.** Everything measured so far has been a one-beat,
TLAST-terminated packet. The old print of "16 stream words" was misleading by
construction — it always printed 16 words regardless of how many the DMA wrote.

### What the ILA window shows

Trigger `TREADY == 1`, depth 1024 at 130 MHz (7.9 us window):

- `TVALID` is high across the whole window, `TREADY` is low for almost all of it,
  `TLAST` is low.

`TVALID` high with `TREADY` low means the arbiter always has a beat ready and the
DMA is not consuming. `pd_axis_fifo` uses `s_tready = (used != DEPTH)` and
`wr_en = s_tvalid && s_tready`, so it **backpressures the producer and does not
drop entries**; `pd_feature_core` stalls accordingly. Nothing is silently lost,
but the transfer also completes within microseconds of being armed — consistent
with the one-beat packet identified above.

### Correction to the earlier bound argument

An earlier draft of this document implied the packet could not exceed the total
FIFO capacity (4 x 256 = 1024 beats). That is wrong. The S2MM packet ends at the
next TLAST, and FIFO depth does not bound how many peak events are produced
between two cycle-stats packets — the FIFOs drain while the packet is still being
formed. The packet length is set by the **event generation rate between
consecutive cycle packets**, a data-rate question rather than a buffering one.
If the four channels' cycle packets happen to cluster in time, the gap can approach
a full 20 ms power cycle.

### Probe delivered

`sw/dma_s2mm_probe.c` (new file; `sw/dma_s2mm_dds_test.c` is left untouched as the
reproduction instrument). Differences from the loop version:

1. BTT `65528` bytes instead of 128.
2. No inter-transfer gap — re-arm immediately, so TREADY stays high.
3. RX buffer mapped non-cacheable via `Xil_SetTlbAttributes` (0x01100000 is 1 MB
   aligned), removing the 64 KB flush/invalidate walk from the re-arm path.
4. `S2MM_LENGTH` read back after **every** completion and recorded as the packet
   length; min / max / sum and a byte-length histogram are kept in RAM.
5. Poll breaks on `Idle || error bits || Halted`, so a real error is reported at
   once instead of after `POLL_LIMIT` spins.
6. Only the words actually written are printed; the first 4 packets and the first
   8 packets longer than one beat are dumped in full.
7. On the first error it prints the decoded DMASR bits, the histogram and a dump,
   then **stops** — no reset, no retry.

Self-check built into the output: a one-beat packet must read back as `8`. If it
reads back as `BTT - 8 = 65520`, the register is behaving as a remaining-count
register in this build and every length must be converted with `BTT - LENGTH`.

### How to run it

1. Keep the current ILA-enabled bitstream programmed; no PL rebuild is needed.
2. In the Vitis application `pd_dma_s2mm_test`, replace
   `src/dma_s2mm_dds_test.c` with `sw/dma_s2mm_probe.c` (rename the file to keep
   the same name, or delete the old source first so that two `main()` functions
   do not collide in the same application component).
3. Improve the ILA trigger before capturing: use `TVALID && TLAST` (packet tail)
   with the trigger position late in the window, or `TVALID && !TREADY`
   (backpressure). `TREADY == 1` alone fires on the first accepted beat and hides
   the rest of the packet.
4. Run for at least a few thousand packets. A packet completes only at a TLAST,
   so the natural cadence is the aggregate cycle-stats rate, roughly 4 per 20 ms
   (about 200 packets/s) — allow tens of seconds.

## Result of the first probe run (2026-09-15)

`sw/dma_s2mm_probe.c`, BTT = 65528, continuous re-arm, ~30,000 packets. `DMASR`
stayed at `0x00001002` (IOC + Idle) throughout: **no error bit was ever set**,
`over = 0`, `zero = 0`.

```text
packets=29825 min=8 max=8224(max@178) avg=1885
hist(bytes<= 8:21863 16:324 32:114 64:425 128:0 256:5 512:8
     1024:11 2048:84 4096:238 8192:117 16384:6636 32768:0 65528:0 over=0 zero=0
```

### What is now established

1. **The `S2MM_LENGTH` readback is the actual byte count.** 21,863 packets read back
   exactly `8`; a remaining-count register would have read about 65,520. The one-shot
   readback of `8` is therefore confirmed as one received beat, and the warning in the
   "Proven hardware evidence" section above is superseded.
2. **The failure mechanism is confirmed.** BTT = 128 = 16 beats was smaller than the
   packet. With BTT = 65528 (8191 beats), packets up to 1028 beats returned with no
   error at all. The decode of `DMASR = 0x5011` as "buffer filled before TLAST, packet
   continued" is the correct one.
3. **The length distribution is bimodal.** 73.3 % of packets are exactly one 8-byte
   beat; 22.3 % are 8193..8224 bytes (1025..1028 beats); the remaining 4.4 % spread
   over 2..4096 bytes, with a hole at 9..16 beats.

### The model that fits the numbers

TLAST is asserted only by the cycle-stats packet, and all four channels share the
50 Hz sync, so their cycle packets arrive clustered at the power-cycle boundary. The
output stream therefore forms **four packets per 20 ms**: the first three are the
trailing cycle packets delivered back to back (one beat each) and the fourth spans the
whole power cycle, so its length is the number of events generated in that cycle.
That predicts:

- 4 packets per power cycle = 200 packets/s;
- 3/4 = 75 % one-beat packets — measured 73.3 %;
- one packet per cycle carrying a full cycle of events — measured 22.3 %.

Every element of the measured distribution matches the prediction.

### What is NOT yet established

Whether the 1025..1028-beat ceiling is **rate-driven** (the true event count in one
power cycle, in which case it scales with discharge activity) or **buffer-driven**
(1024 = the four-channel FIFO capacity, 4 x 256). The near-absence of spread argues for
a hard ceiling; the 3:1 packet ratio and the 1025-versus-1024 coincidence are
consistent with both. This decides whether `BTT = 65528` is engineering margin or
luck: if the ceiling is rate-driven, the buffer must be sized for the worst-case
discharge rate rather than for what was measured today.

### Probe v2

`sw/dma_s2mm_probe_v2.c` answers it using counters already present in the stream, with
no timer:

- `cycle_idx` in the cycle packet increments once per 50 Hz cycle, so it yields
  packets-per-cycle and events-per-cycle directly (`pktsPerCycle_x100` should be ~400).
- `n` in the cycle packet is that cycle's event count. If the total peak-event words
  received equals `sum(n)`, the big packets are literally one power cycle of real
  events and the length is rate-driven.
- Every packet must contain exactly one `type=0x01` word and it must be the **last**
  word. `lastnotcyc` and `multicyc` must both stay 0; anything else falsifies the model.

### Parser fix (2026-09-15): peak-event field map was wrong

While cross-checking the probe against the RTL defines, a real defect surfaced in the
**field decode**, not in the transport logic.

| Source | Peak-event layout |
|---|---|
| `接口契约v3.0.md` §7 | `phase[39:28] polarity[27] ch_id[26:25] event_seq[24:0]` |
| `pd_defines.vh:36` | `` `define PD_PH_FIELD_W 12 `` ("与 B 侧 RTL 一致 … 已定案 2026-09-08") |
| `pd_feature_core.v:574` | comment: peak `ch_id` at `[26:25]`, cycle `ch_id` at `[31:30]` |
| `dma_s2mm_dds_test.c` | **10-bit map**: `(lo>>29)&1`, `(lo>>27)&3`, `lo&0x07ffffff` |

The contract and the RTL agree on the 12-bit map; **only the C parser disagreed**, so
every `phase` / `pol` / `ch` / `seq` it printed was wrong by two bits. The probes
(`dma_s2mm_probe.c`, `dma_s2mm_probe_v2.c`) inherited the same defect.

Both probes are now fixed, and the field map is a single macro block
(`PD_PH_FIELD_W`) that mirrors `pd_defines.vh`, so it can only drift in one place.

**Verification** — the six peak words captured in the run log were re-decoded with an
independent implementation (Python) and then asserted against the C macros on a host
build; all six match:

```text
PD_PH_FIELD_W=12  HISHIFT=4 LOSHIFT=28 LOMASK=0xf POL=27 CH=25 SEQ=0x1ffffff
  raw=00FD2C18_02000024  phase= 384 pol=0 ch=1 seq=36   expect  384 0 1 36  OK
  raw=00002A04_BC00004B  phase=  75 pol=1 ch=2 seq=75   expect   75 1 2 75  OK
  raw=00FD2F08_00000024  phase= 128 pol=0 ch=0 seq=36   expect  128 0 0 36  OK
  raw=00002A04_CC00004C  phase=  76 pol=1 ch=2 seq=76   expect   76 1 2 76  OK
  raw=00FD1438_06000024  phase= 896 pol=0 ch=3 seq=36   expect  896 0 3 36  OK
  raw=00002A04_DC00004D  phase=  77 pol=1 ch=2 seq=77   expect   77 1 2 77  OK
RESULT: all 6 OK
```

Independent corroboration that the fix is right: under the corrected map the three
`00002A04_*` words decode as **channel 2 with consecutive `seq` 75, 76, 77** — exactly
what a per-channel event counter must do. The buggy map returned `ch = 3, 1, 3` for
the same three words, because bits `[26:25]` were being counted inside the sequence
field instead of being read as the channel.

Note the length histogram is unaffected by this: lengths come from `S2MM_LENGTH`, not
from the field decode. The cycle-statistics word layout is also unaffected — it is
fixed at `cycle_idx[55:32] ch_id[31:30] cyc_n[29:16] qmax[15:0]`.

### v3: bounded run, and the 2026-09-15 target-state incident

`sw/dma_s2mm_probe_v3.c` is v2 plus one bound: after `PKT_LIMIT` (20000) packets it
prints the final report and parks the CPU in `WFI` with the S2MM channel no longer
armed. The measurement is otherwise identical, so results stay comparable.

Why this was needed. v2 re-arms with no inter-transfer gap and polls `DMASR` flat out,
so once Vitis releases the CPU (`con`) the target is left running at full load. On
2026-09-15 that produced this, all from the **same build**:

```text
11:45:06  dow F:/ps/pd_dma_s2mm_test/build/pd_dma_s2mm_test.elf
11:45:07  con                                   -> v2 ran, serial output observed   OK
11:45:23  same ELF, rst -processor
11:45:25  ERROR Memory write error at 0x100000. AP transaction timeout
11:45:34+ every retry -> ERROR AP transaction error, DAP status 0xF0000021
```

The identical binary downloaded and ran seventeen seconds earlier. **The binary was not
the variable — the state left on the target was.** Two consequences worth remembering:

1. Everything from the second attempt onward is a **sticky-error echo** of that single
   failed bus transaction. Reading the retries (all `0xF0000021`) tells you nothing; only
   the first error line does.
2. `APB Memory access port is disabled` in the target list is a *CoreSight stopped
   reason*, not a failure by itself. In this incident it was the label attached to the
   failure, and it is the same text that shows up for an unresponsive slave.

Recovery that worked in principle and should be the standing procedure:

1. **Power-cycle the board** — clears the stuck DDR controller, the latched DAP error and
   the PL configuration in one step. Nothing else is meaningful until this is done.
2. **Program the PL bit** (Vivado Hardware Manager, or `fpga <bit>` in xsct). The Vitis run
   configuration has `programDevice: false`, so it does *not* do this for you.
3. Then download and run.

Operational rule, more important than any code change here:

> **Always halt the target (or power-cycle) before re-downloading, and never press Run a
> second time against a board that is still executing the previous probe.** From the
> debugger's side a continuously running instrument is indistinguishable from a board
> that will not respond.

Also noted in `pd_dma_s2mm_test/_ide/.theia/launch.json`: `programDevice: false`,
`resetSystem: false`, and **both** `usingFSBL: true` and `usingPs7Init: true` enabled
simultaneously. AMD guidance is that FSBL already runs `ps7_init`, so only one of the two
should be active.

## Result of the v2 discriminating run (2026-09-15) — the length is RATE-driven

Final report of the bounded run (`dma_s2mm_probe`, 11009 packets):

```text
packets=11009 min=8 max=8232(max@651) avg=1834
hist(bytes<= 8:8058 16:106 32:41 64:251 128:1 256:4 512:4 1024:7
     2048:41 4096:69 8192:44 16384:2383 32768:0 65528:0 over=0 zero=0
acct: peak=2514014 cyc=11009 cycN=2512958 other=0 lastnotcyc=0 multicyc=0
acct: cycles=2779 pktsPerCycle_x100=396 evtsPerCycle=904
acct: polls_min=1 polls_max=79945 maxPktPolls=79801
```

### The question this was built to answer, answered

The pre-registered criterion was: *if the peak words received equal `sum(n)` of the cycle
packets, explanation (A) holds and the length is rate-driven.*

```text
peak = 2514014      cycN = 2512958      difference = 1056  =  0.042 %
```

**They match to within 0.04 %.** The big packets are one power cycle of real peak
events. The length is therefore **rate-driven**, and the 1024-versus-1025 resemblance to
the four-channel FIFO capacity is a coincidence.

Independent second confirmation is in the same report: `polls_min = 1` but
`maxPktPolls = 79801`. A saturated-FIFO drain would complete in tens of polls (1024 beats
at one beat per 130 MHz cycle is about 8 us). 79801 polls is roughly one power cycle of
waiting, which is what a real frame interval looks like and a backlog drain does not.

### The stream model is confirmed exactly, not approximately

Three structural checks, all self-reported by the probe, all clean:

| Counter | Value | Meaning |
|---|---|---|
| `other` | **0** | no word carried an unknown type byte |
| `lastnotcyc` | **0** | every packet's last word is a `type=0x01` cycle packet |
| `multicyc` | **0** | every packet contains exactly one cycle word |

So "the TLAST beat is always the cycle packet, and it always terminates the packet" is
confirmed against real traffic. `cyc = 11009 = packets` follows from `multicyc = 0`.

### New defect found in the RTL while explaining the residual

`pd_feature_core.v:199`, `:211`, `:344-346`:

```verilog
reg  win_pending, cyc_pending;                                   // a FLAG, not a counter
wire fsm_take_cyc = (fsm == S_IDLE) && !win_pending && cyc_pending;
if (cyc_end)            cyc_pending <= 1'b1;
else if (fsm_take_cyc)  cyc_pending <= 1'b0;
```

A cycle boundary that arrives while `cyc_pending` is still set is **swallowed**. The
measurement agrees: `pktsPerCycle_x100 = 396`, i.e. 3.96 cycle packets per boundary where
the design permits 4. About **1 % of channel-cycle slots never emit their cycle packet**;
their events roll into the next cycle's `n` (which is why `peak == cycN` still holds).

Two consequences, both contract-relevant:

1. `cycle_idx` is incremented only inside `S_CYC`, so a swallowed boundary does not
   increment it either. **The PS cannot detect the lost frame from the event stream** —
   it sees a normal `cycle_idx` step carrying roughly twice the usual `n`.
2. The skip rate will rise under heavier load. This is a latent scaling defect for
   the 65 MSPS target, not a current failure.

Contract §7 does not describe this behaviour at all. Together with the missing
packet-length bound, that is **two** gaps in the same section.

### Engineering verdict

Observed maximum packet: 8232 bytes (1029 beats) against `BTT = 65528`, i.e. **8.0x**
headroom — and between the two runs the maximum moved from 8224 to 8232 bytes, i.e. it
tracks the discharge activity. So:

- `BTT = 65528` is a valid **bring-up** configuration.
- It is **not** a structural guarantee, and it is capped by `c_sg_length_width = 16`
  at 65535 bytes. **Production must use option B (PL fixed-length framing)**, or must
  carry a verified worst-case event-rate budget with a documented margin.

### One number still does not add up

`evtsPerCycle = 904` while the big packets average about 1026 peak words
(2383 packets in the `(8192,16384]` bucket, max 8232 bytes, i.e. a tight 1025..1029 beat
cluster). A packet bounded by two adjacent boundaries should carry about one cycle of
events, not 1.13 cycles' worth. Conservation is not violated — the big packets carry
97.4 % of all peak words over 97.4 % of the time — so the discrepancy is in the
**events-per-cycle denominator**, not in the totals. Two candidates remain:
`cycle_idx` advancing at a slightly different rate than the sync (so `cycles` is not
exactly the number of elapsed power cycles), or boundary-to-boundary windows that are not
exactly one cycle.

It is recorded here rather than explained away. The measurement that settles it is to log,
per packet, the `cycle_idx` of the terminating cycle word: that gives the number of
`cycle_idx` units each packet actually spans, instead of inferring it from totals.

### Verdict

The receive path is **functionally correct at BTT = 65528 with the present stimulus**:
~30,000 consecutive packets, no error bits, byte-count semantics verified. It is **not
production-safe**, because the packet length is data-dependent and the only thing
keeping it below BTT is today's discharge rate. Sizing the buffer from a quiet-case
measurement is precisely how `DMASR = 0x5011` was produced in the first place. Treat
BTT = 65528 as a bring-up configuration and take the production decision (option B,
fixed-length framing) once v2 says whether the ceiling is rate-driven.

### ILA note, important for future captures

`pd_axis_arb` drives `m_tvalid = sel_valid` but leaves `m_tdata` / `m_tlast` at
whatever the selected FIFO output happens to hold. `pd_axis_fifo` backs its read
address up when empty (`rd_addr = (num != 0) ? raddr : (raddr - 1)`), so an empty
output register still presents the **last consumed** entry together with its `tlast`
bit. Consequence: whenever `TVALID = 0` the TDATA and TLAST values on the probe are
stale and meaningless — which is exactly why a capture can show `TLAST = 1` with
`TVALID = 0`. Trigger on `TVALID && TLAST`, never on `TREADY` alone, and ignore
TDATA/TLAST outside `TVALID`.

### How to run v2

Same procedure as above, substituting `sw/dma_s2mm_probe_v2.c` for
`sw/dma_s2mm_probe.c`, and **replacing** the active Vitis source rather than adding a
second file: `aux_source_directory` globs every `.c` in `src/`, so two `main()`
functions will not link.
