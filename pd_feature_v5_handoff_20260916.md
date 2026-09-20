# pd_feature v5 handoff - 2026-09-16

## Current objective

Guide the user through personally implementing the PL-side four-slot DDR snapshot manager. The user explicitly does **not** want an agent to silently write the whole feature. Work as a tutor and reviewer: give one small coding milestone at a time, explain the logic, review pasted code/errors, and edit files only if the user explicitly asks.

## Target environment

- Board: 正点原子领航者 Zynq-7020, `xc7z020clg400-2`.
- Vivado: 2020.2 (`F:\vivado20`).
- PS software tool available later: Vitis 2024.1.
- Current project snapshot to open:
  `F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\pd_feature_bd_2020_2.xpr`

## Version-control state

- GitHub repository: `https://github.com/AntonioXu231/jufangyi.git`.
- Latest published release: `v1.3.0`, main merge commit `39ba7c6`.
- Feature branch: `feature/v1.3.0-ddr-1g-address-map`, feature commit `9348f0f`.
- Snapshot above was deliberately created as a new directory; prior snapshots were not overwritten.
- Release record: `F:\xinya\v5\版本管理\变更记录\v1.3.0-ddr-1g-address-map.md`.
- `v1.3.0` records DDR-address mapping only; no post-change behavioral sim, synthesis, implementation, bitstream, XSA regeneration, or board validation has been performed.
- Do not push later work unless the user explicitly asks to push to GitHub. Follow `F:\xinya\v5\版本管理\README.md`, CHANGELOG, and the change-record template for every release.

## Important current hardware facts

- Core-board schematic confirms two x16, 4-Gbit DDR3 devices connected as PS DQ[31:0] / four DQS groups: physical total is 1 GB on a x32 PS interface.
- `PCW_UIPARAM_DDR_BUS_WIDTH = 16 Bit` describes each DDR component and is correct. Do not set it to 32 Bit.
- `PCW_DQ_WIDTH = 32` and `PCW_DQS_WIDTH = 4` describe the aggregate PS interface and are correct.
- Snapshot PS7 DDR visible range has been saved as `0x0010_0000 - 0x3FFF_FFFF`.
- Address Editor mappings for `axi_dma_0/Data_S2MM`, `pd_ddr_0/m_axi_wr`, `pd_ddr_0/m_axi_rd`, and `pd_ddr_0/m_axi_cw` have been saved as 1G, base `0x0000_0000`, high `0x3FFF_FFFF`.
- `scripts/04_restore_v3_axi_address_map.tcl` in the new snapshot was updated so the three `pd_ddr_0` mappings restore at 1G rather than 512M.

## Architecture to preserve

`pd_ddr_0` is custom PL data-storage management, not a DDR controller and not MIG.

```text
DDS/ADC -> pd_ddr_0
  -> pack48 / CDC FIFO / pack192 / AXIS FIFO
  -> dm_wr DataMover -> HP0 -> PS7 hard DDR controller -> DDR3 ring
  -> freeze at cycle boundaries
  -> dm_cp DataMover MM2S+S2MM snapshot copy via HP1
  -> pd_feature_0 / pd_filter_0 -> axi_dma_0 -> HP0 -> DDR (event stream)
```

Existing relevant RTL (do not replace its validated data path):

- `.../imports/rtl/pd_ddr_wr_top.v`
- `.../imports/rtl/pd_ddr_ring_wr.v`
- `.../imports/rtl/pd_ddr_snap_copy.v`
- `.../imports/rtl/pd_ddr_axil.v`
- `.../imports/rtl/pd_ddr_defines.vh`
- `.../imports/rtl/pd_ddr_bd_adapter.v`

Existing IP already serves the needs: AXI DataMover `dm_wr` and `dm_cp`, FIFO Generator, AXIS Data FIFO, SmartConnect, PS7. Do not add MIG or a second DataMover for four slots.

## Existing DDR layout

From `pd_ddr_defines.vh`:

- Ring: base `0x1000_2000`, size roughly 128 MB.
- Legacy single snapshot: base `0x1800_0000`, size roughly 8 MB.

Proposed future four-slot area, not yet implemented:

```text
slot0: 0x2000_0000 - 0x20BF_FFFF  (12 MB)
slot1: 0x20C0_0000 - 0x217F_FFFF  (12 MB)
slot2: 0x2180_0000 - 0x223F_FFFF  (12 MB)
slot3: 0x2240_0000 - 0x22FF_FFFF  (12 MB)
```

At final 65 MSPS x 4 channels x 12 bits, raw rate is 390 MB/s and one 50-Hz 20-ms cycle is 7.8 MB. A 12-MB slot accommodates one full cycle with margin.

## Phase 5 intended functionality

New custom RTL module name: `pd_ddr_slot_mgr.v`.

Lifecycle:

```text
FREE -> RESERVED -> COPYING -> VALID -> FREE
```

- On `freeze_done`, select a FREE slot when `auto_snap_en` is enabled.
- Feed existing `pd_ddr_snap_copy` source base/length and selected slot destination base.
- Set `slot_valid[n]` only after `copy_done`.
- On `copy_err`, leave the slot invalid and set an error status.
- PS later releases a consumed slot with a W1P `slot_release[n]` bit.
- Safe policy: when all slots are VALID/BUSY, reject the new snapshot and set a sticky full indication; never overwrite a valid slot.
- Keep the legacy manual `o_snap_start` path while introducing auto-slot mode.

## Teaching cadence agreed with user

1. The user creates `pd_ddr_slot_mgr.v` in `sources_1/imports/rtl` and adds it to Design Sources.
2. First only write/compile the module interface. No instantiation or integration yet.
3. Then guide the user through: parameters and slot addresses; state/register declarations; sequential state updates; free-slot selection; copy handshake; release handling; integration; AXI-Lite extension; simulation.
4. After every small step, ask user to paste the module or exact error and review it. Do not jump directly to a complete implementation unless asked.

The first suggested skeleton/interface was already given to the user in the prior turn. The next session should begin by asking whether they completed that skeleton and requesting either its content or the Vivado synthesis result.

## AXI-Lite context

`pd_ddr_axil.v` currently uses offsets `0x00` through `0x48`, with `0x48` as `SNAP_SEQ`. Reserve new multi-slot registers from `0x4C` onward. Do not change existing offsets.

Suggested future register direction (not yet committed):

- `0x4C`: SLOT_CTRL (`auto_snap_en`, W1P release bits)
- `0x50`: SLOT_STATUS (valid/busy/full/error)
- `0x54`: SLOT_LAST
- `0x58`: SLOT_SEQ
- `0x60` onward: descriptor words for slots 0-3 (base, length, sequence, info)

## Verification history and current boundary

- Historical `v1.1.0`: on-board DDS + ILA + AXI DMA feature stream was repeatedly observed; DMA status `0x00001002` and many variable packets completed.
- Historical `v1.2.0`: IIR filter integration had behavior test pass and timing closure; see changelog for details.
- Do not report any of that historical evidence as validation of the new 1G mapping or future four-slot implementation.

## Suggested skills

- `xilinx-suite`: before Vivado/Vitis actions and when generating Tcl guidance.
- `sim-bringup`: when the slot-manager testbench is added or a simulation fails.
- `timing-closure`: if post-implementation WNS/TNS becomes negative.
- `ila-hw-debug`: only when the user explicitly returns to board-level ILA capture.
- `codebase-design`: for retaining `pd_ddr_slot_mgr` as a small-interface, deep module.

