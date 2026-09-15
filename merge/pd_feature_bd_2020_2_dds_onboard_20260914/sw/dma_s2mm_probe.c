/*
 * AXI DMA S2MM packet-length probe for the on-board DDS build (2026-09-15).
 *
 * Why this probe exists
 * ---------------------
 * The previous 128-byte fixed-BTT loop reproduced
 *     FAIL: transfer=106 DMACR=00010002 DMASR=00005011 LENGTH=128 polls=200000000
 *
 * DMASR bit 4 (DMAIntErr) is documented in PG021 v7.1 as: "When Scatter Gather
 * is disabled, this error is flagged if any error occurs during Memory write or
 * if the incoming packet is bigger than what is specified in the DMA length
 * register."  S2MM_LENGTH is documented as: "This value must be greater than or
 * equal to the largest expected packet to be received on S2MM AXI4-Stream.
 * Values smaller than the received packet result in undefined behavior."
 *
 * In this design TLAST is asserted only by the cycle-statistics packet
 * (pd_feature_core.v, S_CYC state) and is forwarded through the beat-level
 * round-robin arbiter (pd_axis_arb.v, m_tlast = s_tlast[out_ch]).  One
 * TLAST-delimited packet therefore spans *every beat since the previous
 * cycle-stats packet, aggregated across all four channels*.  Its length is a
 * function of the peak-event rate; it is NOT bounded by the event FIFO depth,
 * because the FIFOs drain while the packet is still being formed.
 *
 * This probe measures that length distribution directly.  It never resets or
 * retries through a real error: on the first error it latches the state and
 * stops so that the evidence survives.
 *
 * Differences from dma_s2mm_dds_test.c
 * ------------------------------------
 *   1. BTT = 65528 bytes instead of 128.  65528 is the largest 8-byte multiple
 *      below MaxTransferLen = 2^16-1 = 65535 (c_sg_length_width = 16 in
 *      pd_feature_bd_axi_dma_0_0.xci).  A larger value is rejected by
 *      XAxiDma_SimpleTransfer() with XST_INVALID_PARAM.
 *   2. No 1 ms gap: the next transfer is armed as soon as the previous one
 *      completes, so TREADY stays high and the event FIFOs drain continuously.
 *   3. The DMA buffer is mapped non-cacheable through the MMU, which removes the
 *      64 KB flush/invalidate walk from the re-arm path.
 *   4. S2MM_LENGTH is read back after *every* completion and recorded as the
 *      actual packet length.  PG021 v7.1 states that register is updated with
 *      the number of bytes actually written on the S2MM AXI4 interface.
 *   5. The completion poll breaks on Idle || error bits || Halted, so a real
 *      error is reported immediately instead of after POLL_LIMIT spins.
 *      XAxiDma_Busy() alone cannot do this: in axidma_v9_18 it is only
 *      "((SR & IDLE) ? FALSE : TRUE)" and never tests Halted, so after a
 *      graceful halt it returns TRUE forever.
 *   6. Only the words actually written are printed.  The previous version always
 *      printed 16 words, so stale DDR content was reported as received data.
 *
 * How to read the result
 * ----------------------
 *   LENGTH readback sanity check: a one-beat packet must read back as 8 bytes.
 *   If instead it reads back as (BTT - 8) = 65520, then in this build the
 *   register behaves as a remaining-count register and every reported length
 *   must be converted with  bytes = BTT - LENGTH  before it means anything.
 *
 * Hardware facts this file relies on (verified in the 20260914 snapshot):
 *   AXI DMA base      : 0x4040_0000
 *   S2MM              : enabled, 64-bit stream, Simple mode (SG excluded)
 *   MaxTransferLen    : 65535 bytes
 *   DDR HP0 aperture  : 0x0000_0000 .. 0x1fff_ffff
 *   Buffer            : 0x0110_0000 (1 MB aligned, inside ps7_ddr_0)
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "xil_printf.h"

/*
 * Vitis 2024 standalone uses System Device Tree (SDT) by default, whose AXI DMA
 * driver identifies an instance by base address rather than by DeviceId.
 */
#if defined(XPAR_XAXIDMA_0_BASEADDR)
/* SDT build: the driver identifies the instance by base address. */
#define DMA_BASE         ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
#define DMA_LOOKUP_ARG   XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR)
/* Legacy non-SDT build: the driver identifies the instance by DeviceId. */
#define DMA_BASE         ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
#define DMA_LOOKUP_ARG   XPAR_AXIDMA_0_DEVICE_ID
#else
#error "AXI DMA base-address macro is absent. Rebuild the Vitis platform from the current XSA."
#endif

#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
#define PS_DDR_BASE       XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
#define PS_DDR_BASE       XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
#error "PS DDR base-address macro is absent. Inspect xparameters.h and update PS_DDR_BASE."
#endif

#define RX_BUFFER_BASE    (PS_DDR_BASE + 0x01000000U)

/*
 * Largest legal Simple-mode BTT for c_sg_length_width = 16, rounded down to an
 * 8-byte multiple (64-bit stream).  Anything larger than 65535 is rejected by
 * the driver before it ever reaches the DMA.
 */
#define S2MM_MAX_BTT      65528U
#define BTT_WORDS         (S2MM_MAX_BTT / 8U)

/*
 * Poll budget per transfer.  A DMASR read over AXI-Lite costs tens of core
 * cycles, so 20,000,000 spins is on the order of a second -- several hundred
 * times the expected TLAST cadence.  It only bounds the wait: a timeout is
 * reported as a timeout, never as a DMA error, so a silent source cannot be
 * mistaken for the failure under investigation.  The previous version used
 * 200,000,000, which took roughly 20 s to expire and made the failure look
 * instantaneous.
 */
#define POLL_LIMIT        20000000U
#define REPORT_PERIOD     128U
#define DUMP_FIRST_N      4U          /* full raw dump for the first N packets  */
#define DUMP_LONG_BYTES   8U          /* dump any packet longer than 1 beat     */
#define DUMP_LONG_MAX     8U          /* ... but at most this many such dumps   */
#define DUMP_WORDS_MAX    16U         /* words printed per dump                 */

/*
 * When RX_NONCACHEABLE is 1 the buffer section is mapped non-cacheable and no
 * cache maintenance is needed.  Set it to 0 to fall back to flush/invalidate.
 */
#define RX_NONCACHEABLE   1

/* AXI DMA S2MM register offsets and status bits (PG021, Simple mode). */
#define S2MM_DMACR_OFFSET  0x30U
#define S2MM_DMASR_OFFSET  0x34U
#define S2MM_LENGTH_OFFSET 0x58U

#define DMASR_HALTED       0x00000001U
#define DMASR_IDLE         0x00000002U
#define DMASR_DMA_INT_ERR  0x00000010U
#define DMASR_DMA_SLV_ERR  0x00000020U
#define DMASR_DMA_DEC_ERR  0x00000040U
#define DMASR_IOC_IRQ      0x00001000U
#define DMASR_ERR_IRQ      0x00004000U
#define DMASR_ERROR_MASK   (DMASR_DMA_INT_ERR | DMASR_DMA_SLV_ERR | \
                            DMASR_DMA_DEC_ERR | DMASR_ERR_IRQ)

/* Byte-length histogram.  Beats are 8 bytes, so the buckets are beats 1..8191. */
#define NBUCKETS 14
static const u32 bucket_hi[NBUCKETS] = {
    8U, 16U, 32U, 64U, 128U, 256U, 512U,
    1024U, 2048U, 4096U, 8192U, 16384U, 32768U, 65528U
};
static u32 hist[NBUCKETS];
static u32 hist_over;      /* > BTT: impossible without an error, kept as a trap */
static u32 hist_zero;      /* a TLAST with no payload must be investigated     */

static XAxiDma AxiDma;
static u32 pkt_count;
static u32 bytes_sum;
static u32 bytes_min;
static u32 bytes_max;
static u32 bytes_max_at;
static u32 dump_long_count;

static inline u32 dma_rd(u32 off)   { return Xil_In32(DMA_BASE + off); }

static void hist_add(u32 bytes)
{
    u32 k;

    if (bytes == 0U) {
        hist_zero++;
        return;
    }
    for (k = 0U; k < NBUCKETS; ++k) {
        if (bytes <= bucket_hi[k]) {
            hist[k]++;
            return;
        }
    }
    hist_over++;
}

static void print_stream_word(u32 index, u64 event_word)
{
    const u32 hi   = (u32)(event_word >> 32);
    const u32 lo   = (u32)event_word;
    const u32 type = (hi >> 24) & 0xffU;

    /* xil_printf handles %x/%d/%u reliably; do not use %l or 64-bit formats. */
    if (type == 0x00U) {
        /* Peak event: [63:56]type [55:40]q [39:30]phase [29]pol [28:27]ch [26:0]seq. */
        const s16 q_q8_8 = (s16)((hi >> 8) & 0xffffU);
        const u32 phase  = ((hi & 0xffU) << 2) | ((lo >> 30) & 0x3U);
        const u32 pol    = (lo >> 29) & 0x1U;
        const u32 ch_id  = (lo >> 27) & 0x3U;
        const u32 seq    = lo & 0x07ffffffU;

        xil_printf("%02u: raw=%08x_%08x PEAK q_q8_8=%d phase=%03u pol=%u ch=%u seq=%u\r\n",
                   index, hi, lo, (s32)q_q8_8, phase, pol, ch_id, seq);
    } else if (type == 0x01U) {
        /* Cycle packet: [63:56]type [55:32]cycle_idx [31:30]ch [29:16]n [15:0]qmax. */
        const u32 cycle_idx = hi & 0x00ffffffU;
        const u32 ch_id     = (lo >> 30) & 0x3U;
        const u32 cyc_n     = (lo >> 16) & 0x3fffU;
        const u32 qmax      = lo & 0xffffU;

        xil_printf("%02u: raw=%08x_%08x CYCLE idx=%u ch=%u n=%u qmax=%u\r\n",
                   index, hi, lo, cycle_idx, ch_id, cyc_n, qmax);
    } else {
        xil_printf("%02u: raw=%08x_%08x unknown_type=%02x\r\n", index, hi, lo, type);
    }
}
static void dump_words(u32 bytes)
{
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
    u32 words = bytes / 8U;
    u32 n, i;

    if (words > DUMP_WORDS_MAX) {
        words = DUMP_WORDS_MAX;
    }
    n = bytes / 8U;
    xil_printf("dump: %u byte(s) = %u beat(s), showing %u\r\n", bytes, n, words);
    for (i = 0U; i < words; ++i) {
        print_stream_word(i, rx[i]);
    }
    if (bytes > DUMP_WORDS_MAX * 8U) {
        xil_printf("dump: (truncated)\r\n");
    }
}

static void print_histogram(void)
{
    u32 k;

    xil_printf("packets=%u min=%u max=%u(max@%u) avg=%u\r\n",
               pkt_count, bytes_min, bytes_max, bytes_max_at,
               pkt_count ? (bytes_sum / pkt_count) : 0U);
    xil_printf("hist(bytes<=");
    for (k = 0U; k < NBUCKETS; ++k) {
        xil_printf(" %u:%u", bucket_hi[k], hist[k]);
    }
    xil_printf(" over=%u zero=%u\r\n", hist_over, hist_zero);
}

int main(void)
{
    XAxiDma_Config *cfg;
    u32 transfer, s2mm_cr, s2mm_sr, s2mm_len, polls, actual;
    int status;

    xil_printf("\r\n--- S2MM packet-length probe (BTT=%u) ---\r\n", S2MM_MAX_BTT);
    xil_printf("RX buffer: 0x%08x, BTT=%u bytes (%u beats)\r\n",
               (u32)RX_BUFFER_BASE, S2MM_MAX_BTT, BTT_WORDS);

#if RX_NONCACHEABLE
    /* 0x01100000 is 1 MB aligned, so it can be mapped as a non-cacheable section. */
    Xil_SetTlbAttributes((INTPTR)RX_BUFFER_BASE, NORM_NONCACHE);
    xil_printf("RX buffer mapped non-cacheable; no cache maintenance needed\r\n");
#endif

    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL) {
        xil_printf("ERROR: XAxiDma_LookupConfig failed\r\n");
        return -1;
    }

    status = XAxiDma_CfgInitialize(&AxiDma, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: XAxiDma_CfgInitialize=%d\r\n", status);
        return -1;
    }
    if (XAxiDma_HasSg(&AxiDma)) {
        xil_printf("ERROR: hardware is SG mode; this Simple-mode probe must not run\r\n");
        return -1;
    }

    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&AxiDma);
    while (!XAxiDma_ResetIsDone(&AxiDma)) {
        /* Wait for the S2MM channel to leave reset. */
    }

    xil_printf("S2MM before arm: DMACR=%08x DMASR=%08x\r\n",
               dma_rd(S2MM_DMACR_OFFSET), dma_rd(S2MM_DMASR_OFFSET));
    xil_printf("Continuous drain, no inter-transfer gap. Arm the ILA now.\r\n");

    bytes_min = 0xFFFFFFFFU;

    for (transfer = 0U; ; ++transfer) {
#if !RX_NONCACHEABLE
        /* Fallback path only: write back then discard stale lines. */
        Xil_DCacheFlushRange((UINTPTR)RX_BUFFER_BASE, S2MM_MAX_BTT);
        Xil_DCacheInvalidateRange((UINTPTR)RX_BUFFER_BASE, S2MM_MAX_BTT);
#endif
        XAxiDma_IntrAckIrq(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

        status = XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)RX_BUFFER_BASE,
                                       S2MM_MAX_BTT, XAXIDMA_DEVICE_TO_DMA);
        if (status != XST_SUCCESS) {
            xil_printf("ERROR: transfer=%u XAxiDma_SimpleTransfer=%d\r\n", transfer, status);
            return -1;
        }

        /*
         * Break as soon as the channel is idle, halted, or signalling an error.
         * XAxiDma_Busy() is deliberately not used here: it only looks at Idle and
         * therefore spins the full POLL_LIMIT after a graceful halt.
         */
        for (polls = 0U; polls < POLL_LIMIT; ++polls) {
            s2mm_sr = dma_rd(S2MM_DMASR_OFFSET);
            if ((s2mm_sr & DMASR_IDLE) || (s2mm_sr & DMASR_HALTED) ||
                (s2mm_sr & DMASR_ERROR_MASK)) {
                break;
            }
        }

        s2mm_cr  = dma_rd(S2MM_DMACR_OFFSET);
        s2mm_sr  = dma_rd(S2MM_DMASR_OFFSET);
        s2mm_len = dma_rd(S2MM_LENGTH_OFFSET);
        actual   = s2mm_len;

        if ((s2mm_sr & DMASR_ERROR_MASK) || (s2mm_sr & DMASR_HALTED)) {
            xil_printf("\r\n*** S2MM ERROR LATCHED - stopping, no reset, no retry ***\r\n");
            xil_printf("FAIL: transfer=%u polls=%u DMACR=%08x DMASR=%08x LENGTH=%u\r\n",
                       transfer, polls, s2mm_cr, s2mm_sr, s2mm_len);
            if (s2mm_sr & DMASR_HALTED)      xil_printf("  Halted           : RS was cleared by the engine\r\n");
            if (s2mm_sr & DMASR_DMA_INT_ERR) xil_printf("  DMAIntErr        : packet bigger than BTT, or memory-write error\r\n");
            if (s2mm_sr & DMASR_DMA_SLV_ERR) xil_printf("  DMASlvErr        : slave error on the memory path\r\n");
            if (s2mm_sr & DMASR_DMA_DEC_ERR) xil_printf("  DMADecErr        : decode error on the memory path\r\n");
            if (s2mm_sr & DMASR_ERR_IRQ)     xil_printf("  Err_Irq          : error interrupt raised for this transfer\r\n");
            if (s2mm_sr & DMASR_IOC_IRQ)     xil_printf("  IOC_Irq          : buffer filled before TLAST was seen\r\n");
            xil_printf("State before the failure:\r\n");
            print_histogram();
            dump_words(s2mm_len);
            xil_printf("Evidence preserved. Terminate the application now.\r\n");
            return -1;
        }

        if ((polls == POLL_LIMIT) && !(s2mm_sr & DMASR_IDLE)) {
            xil_printf("\r\n*** TIMEOUT - no error bits, no completion in %u polls ***\r\n", POLL_LIMIT);
            xil_printf("TIMEOUT: transfer=%u DMACR=%08x DMASR=%08x LENGTH=%u\r\n",
                       transfer, s2mm_cr, s2mm_sr, s2mm_len);
            print_histogram();
            xil_printf("This is NOT the DMA error under investigation. Check that the core is\r\n");
            xil_printf("emitting cycle packets (TLAST) at all, then re-run.\r\n");
            return -1;
        }

        /* Successful completion: LENGTH readback is the packet length in bytes. */
        if ((transfer < DUMP_FIRST_N) ||
            ((actual > DUMP_LONG_BYTES) && (dump_long_count < DUMP_LONG_MAX))) {
            if (actual > DUMP_LONG_BYTES) {
                dump_long_count++;
            }
            xil_printf("transfer=%u bytes=%u beats=%u polls=%u DMASR=%08x\r\n",
                       transfer, actual, actual / 8U, polls, s2mm_sr);
            dump_words(actual);
        }

        if (actual < bytes_min)          bytes_min = actual;
        if (actual > bytes_max) {
            bytes_max    = actual;
            bytes_max_at = transfer;
        }
        bytes_sum += actual;
        hist_add(actual);
        pkt_count++;

        if ((transfer % REPORT_PERIOD) == 0U) {
            xil_printf("completed=%u DMASR=%08x\r\n", pkt_count, s2mm_sr);
            print_histogram();
        }
    }
}
