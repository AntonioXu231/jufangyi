/*
 * Bounded PS-side automatic snapshot smoke test.
 *
 * This test intentionally polls pd_ddr_0 instead of installing a GIC handler.
 * The BD already routes pd_ddr_0/irq to IRQ_F2P[2], but the generated BSP does
 * not expose a named pd_ddr interrupt macro yet. Polling therefore verifies the
 * complete PL/DDR/AXI-Lite contract without guessing an interrupt ID.
 *
 * Required hardware state:
 *   - current pd_feature_bd_wrapper.bit is programmed
 *   - pd_filter_apply has already printed FILTER_APPLY_VERIFY_PASS
 *   - DDS/event source is running and can produce at least one accepted event
 */
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xparameters.h"

#if defined(XPAR_PD_DDR_0_BASEADDR)
#define PD_DDR_BASE              ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
#elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#define PD_DDR_BASE              ((UINTPTR)XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#else
#error "Cannot find pd_ddr AXI-Lite base address in xparameters.h"
#endif

#define DDR_CTRL                 0x000U
#define DDR_STATUS               0x004U
#define FREEZE_CTRL              0x034U
#define SLOT_CTRL                0x04CU
#define SLOT_STATUS              0x050U
#define SLOT_SEQ                 0x054U
#define SLOT_DROPS               0x058U
#define SNAP_TRIG_CTRL           0x05CU
#define SNAP_TRIG_DROPS          0x0A0U

#define SLOT_BASE_OFF(slot)      (0x060U + ((slot) * 0x10U))
#define SLOT_LEN_OFF(slot)       (0x064U + ((slot) * 0x10U))
#define SLOT_SEQ_OFF(slot)       (0x068U + ((slot) * 0x10U))
#define SLOT_FLAGS_OFF(slot)     (0x06CU + ((slot) * 0x10U))

#define DDR_SLOT0_BASE           0x20001000U
#define DDR_SLOT_SIZE            0x00C00000U
#define DDR_SLOT_AREA_END        0x23001000U

#define DDR_STATUS_COPY_BUSY     (1U << 1)
#define DDR_STATUS_COPY_DONE     (1U << 6)
#define DDR_STATUS_COPY_ERR      (1U << 8)
#define DDR_STATUS_CFG_ERR       (1U << 9)

#define SLOT_VALID_MASK          0x0000000FU
#define SLOT_BUSY_MASK           0x000000F0U
#define SLOT_LOCKED_MASK         0x00000F00U
#define SLOT_FULL                (1U << 12)
#define SLOT_CFG_ERR             (1U << 13)
#define SLOT_REQ_OVERFLOW        (1U << 14)
#define SLOT_CMD_ERR             (1U << 15)
#define SLOT_PENDING             (1U << 16)
#define SLOT_LAST_SHIFT          17U
#define SLOT_READY               (1U << 19)

#define TRIG_ENABLED             (1U << 0)
#define TRIG_MASK_ALL            (0xFU << 1)
#define TRIG_ARMED               (1U << 16)
#define TRIG_DROP                (1U << 17)

#define SNAPSHOT_TIMEOUT         120000000U
#define POLL_PRINT_PERIOD        10000000U

static u32 reg_read(u32 off)
{
    return Xil_In32(PD_DDR_BASE + off);
}

static void reg_write(u32 off, u32 value)
{
    Xil_Out32(PD_DDR_BASE + off, value);
}

static void fail_stop(const char *reason);

static void verify_register_contract(void)
{
    u32 ring_base = reg_read(0x08U);
    u32 ring_size = reg_read(0x0CU);
    u32 snap_base = reg_read(0x20U);
    u32 snap_size = reg_read(0x24U);

    xil_printf("DDR_BASE=0x%08x RING=0x%08x/%u SNAP=0x%08x/%u\r\n",
               (u32)PD_DDR_BASE, ring_base, ring_size, snap_base, snap_size);
    if (ring_base != 0x10002000U || ring_size != 0x07FFE000U)
        fail_stop("unexpected ring address contract");
    if (snap_base != 0x18000000U || snap_size != 0x007FE000U)
        fail_stop("unexpected legacy snapshot contract");
}

static void fail_stop(const char *reason)
{
    xil_printf("SNAPSHOT_FAIL: %s\r\n", reason);
    xil_printf("DDR_STATUS=0x%08x SLOT_STATUS=0x%08x TRIG=0x%08x\r\n",
               reg_read(DDR_STATUS), reg_read(SLOT_STATUS),
               reg_read(SNAP_TRIG_CTRL));
    xil_printf("SLOT_DROPS=0x%08x TRIG_DROPS=0x%08x\r\n",
               reg_read(SLOT_DROPS), reg_read(SNAP_TRIG_DROPS));
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

static u32 wait_snapshot_ready(void)
{
    u32 i;
    for (i = 0; i < SNAPSHOT_TIMEOUT; ++i) {
        u32 status = reg_read(SLOT_STATUS);
        u32 ddr_status = reg_read(DDR_STATUS);
        if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR))
            fail_stop("slot manager error");
        if (ddr_status & (DDR_STATUS_COPY_ERR | DDR_STATUS_CFG_ERR))
            fail_stop("DDR copy/config error");
        if (status & SLOT_READY)
            return status;
        if ((i % POLL_PRINT_PERIOD) == 0U)
            xil_printf("WAIT i=%u DDR_STATUS=0x%08x SLOT_STATUS=0x%08x\r\n",
                       i, ddr_status, status);
    }
    fail_stop("snapshot timeout");
    return 0U;
}

static void verify_slot(u32 slot, u32 status)
{
    u32 base = reg_read(SLOT_BASE_OFF(slot));
    u32 len = reg_read(SLOT_LEN_OFF(slot));
    u32 seq = reg_read(SLOT_SEQ_OFF(slot));
    u32 flags = reg_read(SLOT_FLAGS_OFF(slot));
    u32 *data;
    u32 first, last;

    xil_printf("SNAPSHOT slot=%u base=0x%08x len=%u seq=%u flags=0x%08x\r\n",
               slot, base, len, seq, flags);

    if ((status & SLOT_VALID_MASK) == 0U)
        fail_stop("ready asserted without valid slot");
    if ((base & 0x7U) != 0U || (len & 0x7U) != 0U || len == 0U)
        fail_stop("invalid slot alignment or length");
    if (base < DDR_SLOT0_BASE || base >= DDR_SLOT_AREA_END ||
        len > DDR_SLOT_SIZE || base + len > DDR_SLOT_AREA_END)
        fail_stop("slot range outside fixed DDR area");
    if ((status & SLOT_BUSY_MASK) != 0U)
        fail_stop("slot remains busy after snapshot_ready");

    /* Lock selected slot: SLOT_CTRL[7:4] is a W1P field, byte 0. */
    reg_write(SLOT_CTRL, 1U << (4U + slot));
    status = reg_read(SLOT_STATUS);
    if ((status & (1U << (8U + slot))) == 0U)
        fail_stop("slot lock did not stick");

    Xil_DCacheInvalidateRange((UINTPTR)base, len);
    data = (u32 *)(UINTPTR)base;
    first = data[0];
    last = data[(len / sizeof(u32)) - 1U];
    xil_printf("SNAPSHOT_DATA first=0x%08x last=0x%08x\r\n", first, last);

    /* Release selected slot: SLOT_CTRL[11:8] is W1P, byte 1. */
    reg_write(SLOT_CTRL, 1U << (8U + slot));
    status = reg_read(SLOT_STATUS);
    if ((status & (1U << (8U + slot))) != 0U)
        fail_stop("slot release did not clear lock");
}

int main(void)
{
    u32 status, trig, seq_before, seq_after, slot;

    xil_printf("--- pd_snapshot_poll automatic snapshot test ---\r\n");
    verify_register_contract();

    /* Clear stale sticky status before enabling a new bounded run. */
    reg_write(SLOT_CTRL, (1U << 12));
    reg_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED);

    /* Enable ring acquisition and four-slot automatic copying. */
    reg_write(DDR_CTRL, 1U);
    reg_write(SLOT_CTRL, 1U);
    reg_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);

    trig = reg_read(SNAP_TRIG_CTRL);
    status = reg_read(SLOT_STATUS);
    seq_before = reg_read(SLOT_SEQ);
    xil_printf("ARM trig=0x%08x slot=0x%08x seq=%u\r\n",
               trig, status, seq_before);

    if ((trig & (TRIG_ENABLED | TRIG_MASK_ALL)) !=
        (TRIG_ENABLED | TRIG_MASK_ALL))
        fail_stop("automatic event trigger did not enable");
    if ((trig & TRIG_ARMED) == 0U)
        fail_stop("automatic event trigger is not armed");
    if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR))
        fail_stop("slot manager reported stale error");

    xil_printf("WAIT_EVENT: produce one DDS/feature event now\r\n");
    status = wait_snapshot_ready();
    slot = (status >> SLOT_LAST_SHIFT) & 0x3U;
    verify_slot(slot, status);

    seq_after = reg_read(SLOT_SEQ);
    if (seq_after <= seq_before)
        fail_stop("snapshot sequence did not advance");

    /* Resume is a W1P at FREEZE_CTRL[1]; this rearms event trigger logic. */
    reg_write(FREEZE_CTRL, 2U);
    trig = reg_read(SNAP_TRIG_CTRL);
    if ((trig & TRIG_ARMED) == 0U)
        fail_stop("freeze_resume did not rearm trigger");

    xil_printf("SNAPSHOT_POLL_PASS slot=%u seq=%u\r\n", slot, seq_after);
    xil_printf("STOP: CPU is parked; halt before downloading another ELF.\r\n");
    for (;;) {
        __asm__ volatile ("wfi");
    }
    return 0;
}
