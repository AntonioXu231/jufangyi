/*
 * pd_capture_service.c
 *
 * First integration-grade PS capture service for the current DDS prototype.
 *
 * It combines the two proven smoke-test paths without changing uncalibrated
 * PL algorithm settings:
 *
 *   feature AXI4-Stream -> AXI DMA S2MM -> PS DDR
 *   event_accept -> automatic raw-DDR snapshot -> slot descriptor -> PS
 *
 * The program deliberately does NOT write pd_feature thresholds or filter
 * coefficients.  Run pd_filter_apply.c separately only after the coefficients
 * have been approved.  This service owns only the DDR capture/slot and DMA
 * receive contracts.
 *
 * Vitis standalone use:
 *   1. Create an Empty Application from the XSA matching the programmed bit.
 *   2. Replace src/main.c with this source and build.
 *   3. Program the matching bitstream, then launch this ELF.
 *   4. Allow DDS to produce events.  Expected end marker:
 *        CAPTURE_SERVICE_PASS packets=128 snapshots=<nonzero>
 *
 * It is intentionally bounded and leaves S2MM idle before parking, so a user
 * can safely stop the CPU and download a subsequent ELF.
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"

#if defined(XPAR_PD_DDR_0_BASEADDR)
# define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
#elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
# define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#else
# error "Cannot find pd_ddr AXI-Lite base address in xparameters.h"
#endif

#if defined(XPAR_XAXIDMA_0_BASEADDR)
# define DMA_BASE       ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
# define DMA_BASE       ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG XPAR_AXIDMA_0_DEVICE_ID
#else
# error "No AXI-DMA instance in xparameters.h: regenerate the Vitis platform from the current XSA."
#endif

#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
# define PS_DDR_BASE XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
# define PS_DDR_BASE XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
# error "No PS DDR base macro in xparameters.h: inspect the generated BSP."
#endif

/* pd_ddr_0 AXI-Lite register map. */
#define DDR_CTRL             0x000U
#define DDR_STATUS           0x004U
#define FREEZE_CTRL          0x034U
#define SLOT_CTRL            0x04CU
#define SLOT_STATUS          0x050U
#define SLOT_SEQ             0x054U
#define SLOT_DROPS           0x058U
#define SNAP_TRIG_CTRL       0x05CU
#define SLOT_BASE_OFF(n)     (0x060U + ((n) * 0x10U))
#define SLOT_LEN_OFF(n)      (0x064U + ((n) * 0x10U))
#define SLOT_SEQ_OFF(n)      (0x068U + ((n) * 0x10U))
#define SLOT_FLAGS_OFF(n)    (0x06CU + ((n) * 0x10U))

#define DDR_STATUS_ERR       (1U << 5)
#define DDR_STATUS_COPY_ERR  (1U << 8)
#define DDR_STATUS_CFG_ERR   (1U << 9)
#define SLOT_VALID_MASK      0x0000000FU
#define SLOT_BUSY_MASK       0x000000F0U
#define SLOT_FULL            (1U << 12)
#define SLOT_CFG_ERR         (1U << 13)
#define SLOT_REQ_OVERFLOW    (1U << 14)
#define SLOT_CMD_ERR         (1U << 15)
#define SLOT_LAST_SHIFT      17U
#define SLOT_READY           (1U << 19)
#define TRIG_ENABLED         (1U << 0)
#define TRIG_MASK_ALL        (0xFU << 1)
#define TRIG_ARMED           (1U << 16)

/* AXI DMA PG021 S2MM register bank. */
#define S2MM_DMACR_OFFSET    0x30U
#define S2MM_DMASR_OFFSET    0x34U
#define S2MM_LENGTH_OFFSET   0x58U
#define DMASR_HALTED         0x00000001U
#define DMASR_IDLE           0x00000002U
#define DMASR_ERROR_MASK     0x00004070U

#define RX_BUFFER_BASE       ((UINTPTR)PS_DDR_BASE + 0x01000000U)
#define S2MM_MAX_BTT         65528U
#define PACKET_LIMIT         128U
#define DMA_POLL_LIMIT       20000000U
#define SNAP_SLOT_SIZE       0x00C00000U
#define SNAP_SLOT_LOW        0x20001000U
#define SNAP_SLOT_HIGH       0x23001000U
#define SHUTDOWN_POLL_LIMIT  1000000U

static XAxiDma g_dma;

static u32 ddr_read(u32 off) { return Xil_In32(PD_DDR_BASE + off); }
static void ddr_write(u32 off, u32 value) { Xil_Out32(PD_DDR_BASE + off, value); }
static u32 dma_read(u32 off) { return Xil_In32(DMA_BASE + off); }

static void stop_fail(const char *reason)
{
    xil_printf("CAPTURE_SERVICE_FAIL: %s\r\n", reason);
    xil_printf("DDR_STATUS=%08x SLOT_STATUS=%08x SLOT_DROPS=%u\r\n",
               ddr_read(DDR_STATUS), ddr_read(SLOT_STATUS), ddr_read(SLOT_DROPS));
    xil_printf("DMACR=%08x DMASR=%08x LENGTH=%u\r\n",
               dma_read(S2MM_DMACR_OFFSET), dma_read(S2MM_DMASR_OFFSET),
               dma_read(S2MM_LENGTH_OFFSET));
    for (;;) __asm__ volatile ("wfi");
}

/* Start from a known slot/ring state.  This only clears W1P status and enables
 * capture; it does not program feature thresholds or filter coefficients. */
static void configure_capture(void)
{
    u32 i;

    ddr_write(DDR_CTRL, 0U);
    ddr_write(FREEZE_CTRL, 2U);
    for (i = 0U; i < 1000000U; ++i) {
        if ((ddr_read(DDR_STATUS) & (1U | DDR_STATUS_ERR)) == 0U)
            break;
    }
    if (i == 1000000U)
        stop_fail("stale ring error did not clear");

    /* Clear slot/trig sticky state, then arm all four event channels. */
    ddr_write(SLOT_CTRL, (1U << 12));
    ddr_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED);
    ddr_write(DDR_CTRL, 1U);
    ddr_write(SLOT_CTRL, 1U);
    ddr_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);

    if ((ddr_read(SNAP_TRIG_CTRL) & (TRIG_ENABLED | TRIG_ARMED)) !=
        (TRIG_ENABLED | TRIG_ARMED))
        stop_fail("automatic snapshot trigger did not arm");
}

/* A READY transition gives ownership of exactly one slot.  Lock before
 * touching DDR, invalidate the cache, report a bounded sample, release, then
 * resume the ring so the next event can produce another snapshot. */
static void service_snapshot(u32 *last_seq, u32 *snapshot_count)
{
    u32 status = ddr_read(SLOT_STATUS);
    u32 seq = ddr_read(SLOT_SEQ);
    u32 slot, base, len, slot_seq, flags, first, last;
    volatile u32 *words;

    if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR))
        stop_fail("slot manager reported an error");
    if (ddr_read(DDR_STATUS) & (DDR_STATUS_COPY_ERR | DDR_STATUS_CFG_ERR))
        stop_fail("DDR copy/configuration error");
    if (!(status & SLOT_READY) || seq == *last_seq)
        return;

    slot = (status >> SLOT_LAST_SHIFT) & 0x3U;
    if (!(status & (1U << slot)))
        stop_fail("READY without a valid selected slot");

    ddr_write(SLOT_CTRL, 1U << (4U + slot));       /* lock W1P */
    status = ddr_read(SLOT_STATUS);
    if (!(status & (1U << (8U + slot))))
        stop_fail("slot lock did not stick");

    base = ddr_read(SLOT_BASE_OFF(slot));
    len = ddr_read(SLOT_LEN_OFF(slot));
    slot_seq = ddr_read(SLOT_SEQ_OFF(slot));
    flags = ddr_read(SLOT_FLAGS_OFF(slot));
    if ((base & 7U) || (len & 7U) || !len || len > SNAP_SLOT_SIZE ||
        base < SNAP_SLOT_LOW || base + len > SNAP_SLOT_HIGH)
        stop_fail("invalid slot descriptor");

    Xil_DCacheInvalidateRange((UINTPTR)base, len);
    words = (volatile u32 *)(UINTPTR)base;
    first = words[0];
    last = words[(len / 4U) - 1U];
    xil_printf("SNAP %u slot=%u base=%08x len=%u seq=%u flags=%08x first=%08x last=%08x\r\n",
               *snapshot_count, slot, base, len, slot_seq, flags, first, last);

    ddr_write(SLOT_CTRL, 1U << (8U + slot));       /* release W1P */
    if (ddr_read(SLOT_STATUS) & (1U << (8U + slot)))
        stop_fail("slot release did not clear lock");
    ddr_write(FREEZE_CTRL, 2U);                     /* resume/rearm W1P */

    *last_seq = seq;
    (*snapshot_count)++;
}

static void receive_one_packet(u32 packet_index, u32 *peak_words, u32 *cycle_words,
                               u32 *bytes_total)
{
    u32 i, bytes, dmasr, words;
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;

    Xil_DCacheFlushRange(RX_BUFFER_BASE, S2MM_MAX_BTT);
    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, S2MM_MAX_BTT);
    XAxiDma_IntrAckIrq(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    if (XAxiDma_SimpleTransfer(&g_dma, RX_BUFFER_BASE, S2MM_MAX_BTT,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS)
        stop_fail("XAxiDma_SimpleTransfer failed");

    for (i = 0U; i < DMA_POLL_LIMIT; ++i)
        if (!XAxiDma_Busy(&g_dma, XAXIDMA_DEVICE_TO_DMA)) break;
    dmasr = dma_read(S2MM_DMASR_OFFSET);
    bytes = dma_read(S2MM_LENGTH_OFFSET);
    if (i == DMA_POLL_LIMIT || (dmasr & (DMASR_HALTED | DMASR_ERROR_MASK)) ||
        !(dmasr & DMASR_IDLE) || !bytes || bytes > S2MM_MAX_BTT || (bytes & 7U))
        stop_fail("S2MM completion contract failed");

    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
    words = bytes / 8U;
    for (i = 0U; i < words; ++i) {
        u32 type = (u32)(rx[i] >> 56);
        if (type == 0U) (*peak_words)++;
        else if (type == 1U) (*cycle_words)++;
        else stop_fail("unknown event word type");
    }
    if (((u32)(rx[words - 1U] >> 56)) != 1U)
        stop_fail("AXIS packet did not terminate on cycle statistics");
    *bytes_total += bytes;
    if ((packet_index < 2U) || ((packet_index & 0x0fU) == 0U))
        xil_printf("DMA pkt=%u bytes=%u words=%u\r\n", packet_index, bytes, words);
}

/* The receive DMA being idle does not imply that the independent raw-DDR
 * snapshot copier has stopped.  Disable new automatic captures first, then
 * disable ring acquisition and wait for every busy slot to retire.  READY is
 * intentionally not part of this condition: it means a descriptor is valid,
 * not that a transfer remains in flight. */
static void shutdown_capture_cleanly(void)
{
    u32 i;

    ddr_write(SNAP_TRIG_CTRL, 0U); /* disable event-trigger admission */
    ddr_write(DDR_CTRL, 0U);       /* stop raw-ring acquisition */
    ddr_write(FREEZE_CTRL, 2U);    /* release/rearm any freeze state */

    for (i = 0U; i < SHUTDOWN_POLL_LIMIT; ++i) {
        u32 ddr_status = ddr_read(DDR_STATUS);
        u32 slot_status = ddr_read(SLOT_STATUS);
        if (((ddr_status & (1U << 1)) == 0U) &&
            ((slot_status & SLOT_BUSY_MASK) == 0U)) {
            xil_printf("CLEAN_STOP ddr_status=%08x slot_status=%08x polls=%u\r\n",
                       ddr_status, slot_status, i);
            return;
        }
    }
    stop_fail("raw DDR capture did not quiesce after stop request");
}

int main(void)
{
    XAxiDma_Config *cfg;
    u32 packet, last_seq, snapshots = 0U, peaks = 0U, cycles = 0U, bytes = 0U;

    xil_printf("\r\n--- pd_capture_service bounded integration run ---\r\n");
    xil_printf("DDR=%08x DMA=%08x RX=%08x\r\n",
               (u32)PD_DDR_BASE, (u32)DMA_BASE, (u32)RX_BUFFER_BASE);

    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL) stop_fail("XAxiDma_LookupConfig returned NULL");
    if (XAxiDma_CfgInitialize(&g_dma, cfg) != XST_SUCCESS) stop_fail("DMA init failed");
    if (XAxiDma_HasSg(&g_dma)) stop_fail("expected AXI DMA Simple mode");
    XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&g_dma);
    while (!XAxiDma_ResetIsDone(&g_dma)) { }

    configure_capture();
    last_seq = ddr_read(SLOT_SEQ);
    xil_printf("ARMED: slot=%08x trig=%08x seq=%u\r\n",
               ddr_read(SLOT_STATUS), ddr_read(SNAP_TRIG_CTRL), last_seq);

    for (packet = 0U; packet < PACKET_LIMIT; ++packet) {
        receive_one_packet(packet, &peaks, &cycles, &bytes);
        service_snapshot(&last_seq, &snapshots);
    }
    service_snapshot(&last_seq, &snapshots);

    shutdown_capture_cleanly();

    xil_printf("CAPTURE_SERVICE_PASS packets=%u snapshots=%u peak_words=%u ",
               PACKET_LIMIT, snapshots, peaks);
    xil_printf("cycle_words=%u bytes_avg=%u slot_status=%08x drops=%u\r\n",
               cycles, bytes / PACKET_LIMIT, ddr_read(SLOT_STATUS), ddr_read(SLOT_DROPS));
    xil_printf("STOP: DMA is idle; halt CPU before downloading another ELF.\r\n");
    for (;;) __asm__ volatile ("wfi");
    return 0;
}
