/*
 * AXI DMA S2MM stream-consistency probe, v2 (2026-09-15).
 *
 * v1 (dma_s2mm_probe.c) answered the first question and its result is recorded in
 * HANDOFF_2026-09-15_DMA_ILA.md: with BTT = 65528 the channel ran ~30,000 packets
 * with zero error bits, and S2MM_LENGTH read back as the real byte count (min = 8,
 * i.e. one beat, so the register is "bytes written", not "bytes remaining").
 *
 * Observed length distribution (29,825 packets, max = 8224 B at transfer 178):
 *     8 B (1 beat)  : 21863   (73.3 %)
 *     intermediate  : 4.4 % in total, with a hole at 9..16 beats
 *     8193..8224 B  : 6636    (22.3 %, i.e. 1025..1028 beats)
 *
 * v2 exists to settle what that distribution *means*, because it decides whether a
 * fixed large BTT is safe or merely lucky. Two competing explanations:
 *
 *   (A) Rate-driven.  The four channels share the 50 Hz sync, so their cycle-stats
 *       packets (the only beats that carry TLAST) arrive clustered at the cycle
 *       boundary. Per 20 ms the stream therefore forms four packets: three of length
 *       1 beat and one spanning the whole power cycle, whose length is the number of
 *       peak events generated in that cycle. That gives 4 packets/cycle and 75 %
 *       tiny packets -- matching the measured 73.3 % -- and a length that grows
 *       linearly with the discharge rate.
 *   (B) Buffer-driven.  1024 beats is exactly the four-channel FIFO capacity
 *       (4 x 256), so the big packets could be a saturated-backlog drain. That would
 *       make the ceiling structural but coupled to a FIFO parameter.
 *
 * v2 discriminates them using data already present in the stream, with no timer:
 *
 *   - The cycle packet carries `cycle_idx`, a free-running counter incremented once
 *     per 50 Hz power cycle, and `n`, the number of events counted in that cycle.
 *     So cycles elapsed = max(cycle_idx) - min(cycle_idx), and events per cycle =
 *     sum(n) / cycles. If the peak words received equal sum(n), explanation (A) is
 *     confirmed outright: the big packets really are one power cycle of real events.
 *   - The TLAST contract says every packet contains exactly one type=0x01 word and
 *     that it is the LAST word. Any violation is reported, not hidden.
 *   - `polls` for the longest packet is reported separately as an independent check
 *     on how long the DMA waited.
 *
 * Fix history
 * -----------
 * 2026-09-15: peak-event field extraction corrected.
 *   The original dma_s2mm_dds_test.c decoded the peak word with the 10-bit phase
 *   map, but this build uses PD_PH_FIELD_W = 12.  Every phase / pol / ch / seq it
 *   printed was therefore wrong by two bits, and v2 inherited the same defect.
 *   The field map is now a single macro block (PD_PH_FIELD_W, below) that mirrors
 *   pd_defines.vh; change it in one place only, and only together with the RTL.
 *
 * How to read the result
 * ----------------------
 *   acct: peak=N cyc=M cycN=E ...
 *     peak = total type=0x00 (peak event) words received.
 *     cycN = total event count reported by the type=0x01 cycle packets.
 *     If peak ~= cycN then (A) holds: the big packets are one power cycle of real
 *     peak events, so the required BTT scales with the discharge rate and
 *     BTT = 65528 buys headroom only -- it is not a structural guarantee.
 *     If peak >> cycN then (B) is alive and the ceiling is the FIFO depth.
 *   lastnotcyc and multicyc must both be 0, else the TLAST contract model is wrong.
 *   pktsPerCycle_x100 should be about 400 (four packets per power cycle).
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "xil_printf.h"

#if defined(XPAR_XAXIDMA_0_BASEADDR)
#define DMA_BASE         ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
#define DMA_LOOKUP_ARG   XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR)
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

/* Largest legal Simple-mode BTT for c_sg_length_width = 16, 8-byte aligned. */
#define S2MM_MAX_BTT      65528U

/*
 * Poll budget per transfer.  A DMASR read over AXI-Lite costs tens of core cycles,
 * so 20,000,000 spins is on the order of a second -- far more than one 50 Hz power
 * cycle.  It only bounds the wait; a timeout is reported as a timeout, never as a
 * DMA error.
 */
#define POLL_LIMIT        20000000U

#define REPORT_PERIOD     128U
#define DUMP_FIRST_N      2U          /* full raw dump for the first N packets */
#define DUMP_LONG_BYTES   8U          /* dump any packet longer than 1 beat    */
#define DUMP_LONG_MAX     4U          /* ... but at most this many such dumps  */
#define DUMP_WORDS_MAX    8U          /* words printed per dump                */

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

/*
 * ---- Peak-event field map: single source of truth -------------------------
 * Must match the build, not a memory of it:
 *   pd_defines.vh:36          `define PD_PH_FIELD_W   12
 *   pd_feature_core.v:546     localparam PH_FIELD_W = `PD_PH_FIELD_W
 *   接口契约v3.0.md §7        phase[39:28] polarity[27] ch_id[26:25] event_seq[24:0]
 *   pd_feature_core.v:574     comment: peak ch_id at [26:25], cycle ch_id at [31:30]
 *
 *   PD_PH_FIELD_W = 12 -> phase[39:28] polarity[27] ch_id[26:25] event_seq[24:0]
 *   PD_PH_FIELD_W = 10 -> phase[39:30] polarity[29] ch_id[28:27] event_seq[26:0]
 *
 * The cycle-statistics word (type=0x01) is NOT affected by this: its layout is
 * fixed at cycle_idx[55:32] ch_id[31:30] cyc_n[29:16] qmax[15:0].
 */
#define PD_PH_FIELD_W   12

#if (PD_PH_FIELD_W == 12)
#define EV_PH_HISHIFT   4U
#define EV_PH_LOSHIFT   28U
#define EV_PH_LOMASK    0x0000000fU
#define EV_POL_SHIFT    27U
#define EV_CH_SHIFT     25U
#define EV_SEQ_MASK     0x01ffffffU
#elif (PD_PH_FIELD_W == 10)
#define EV_PH_HISHIFT   2U
#define EV_PH_LOSHIFT   30U
#define EV_PH_LOMASK    0x00000003U
#define EV_POL_SHIFT    29U
#define EV_CH_SHIFT     27U
#define EV_SEQ_MASK     0x07ffffffU
#else
#error "PD_PH_FIELD_W must be 10 or 12 to match pd_defines.vh"
#endif

#define NBUCKETS 14
static const u32 bucket_hi[NBUCKETS] = {
    8U, 16U, 32U, 64U, 128U, 256U, 512U,
    1024U, 2048U, 4096U, 8192U, 16384U, 32768U, 65528U
};
static u32 hist[NBUCKETS];
static u32 hist_over;
static u32 hist_zero;

static XAxiDma AxiDma;
static u32 pkt_count;
static u32 bytes_sum;
static u32 bytes_min;
static u32 bytes_max;
static u32 bytes_max_at;

/* ---- stream accounting: check the received stream against the RTL contract ---- */
static u32 acct_peak_words;
static u32 acct_cyc_words;
static u32 acct_cyc_n_sum;
static u32 acct_other_words;
static u32 acct_last_not_cycle;   /* must stay 0: the TLAST beat is the last word  */
static u32 acct_multi_cyc;        /* must stay 0: exactly one TLAST beat per packet */
static u32 acct_cyc_idx_min;
static u32 acct_cyc_idx_max;
static u32 acct_cyc_idx_seen;
static u32 polls_min;
static u32 polls_max;
static u32 bytes_max_polls;

static u32 dump_long_count;

static inline u32 dma_rd(u32 off) { return Xil_In32(DMA_BASE + off); }

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

    if (type == 0x00U) {
        const s16 q_q8_8 = (s16)((hi >> 8) & 0xffffU);
        const u32 phase  = ((hi & 0xffU) << EV_PH_HISHIFT) |
                           ((lo >> EV_PH_LOSHIFT) & EV_PH_LOMASK);
        const u32 pol    = (lo >> EV_POL_SHIFT) & 0x1U;
        const u32 ch_id  = (lo >> EV_CH_SHIFT) & 0x3U;
        const u32 seq    = lo & EV_SEQ_MASK;

        xil_printf("%02u: raw=%08x_%08x PEAK q_q8_8=%d phase=%04u pol=%u ch=%u seq=%u\r\n",
                   index, hi, lo, (s32)q_q8_8, phase, pol, ch_id, seq);
    } else if (type == 0x01U) {
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
    u32 i;

    if (words > DUMP_WORDS_MAX) {
        words = DUMP_WORDS_MAX;
    }
    xil_printf("dump: %u byte(s) = %u beat(s), showing %u\r\n",
               bytes, bytes / 8U, words);
    for (i = 0U; i < words; ++i) {
        print_stream_word(i, rx[i]);
    }
    if (bytes > DUMP_WORDS_MAX * 8U) {
        xil_printf("dump: (truncated)\r\n");
    }
}

/*
 * Walk every beat the DMA actually wrote and reconcile it with the RTL contract.
 * This is the measurement that separates a rate-driven packet length from a
 * FIFO-limited one, so it is done on every packet, not on a sample.
 */
static void account_packet(u32 bytes, u32 polls)
{
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
    u32 words = bytes / 8U;
    u32 cyc_here = 0U;
    u32 last_type = 0xffU;
    u32 i;

    for (i = 0U; i < words; ++i) {
        const u64 w    = rx[i];
        const u32 hi   = (u32)(w >> 32);
        const u32 lo   = (u32)w;
        const u32 type = (hi >> 24) & 0xffU;

        last_type = type;
        if (type == 0x00U) {
            acct_peak_words++;
        } else if (type == 0x01U) {
            u32 idx;
            acct_cyc_words++;
            acct_cyc_n_sum += (lo >> 16) & 0x3fffU;
            idx = hi & 0x00ffffffU;
            if (acct_cyc_idx_seen == 0U) {
                acct_cyc_idx_min = idx;
                acct_cyc_idx_max = idx;
                acct_cyc_idx_seen = 1U;
            } else {
                /* cycle_idx wraps at 2^24; compare modulo 2^24 so a wrap is safe. */
                if (idx > acct_cyc_idx_max &&
                    (idx - acct_cyc_idx_max) < 0x800000U) {
                    acct_cyc_idx_max = idx;
                }
            }
            cyc_here++;
        } else {
            acct_other_words++;
        }
    }

    if ((words != 0U) && (last_type != 0x01U)) {
        acct_last_not_cycle++;
    }
    if (cyc_here > 1U) {
        acct_multi_cyc++;
    }

    if (polls < polls_min) polls_min = polls;
    if (polls > polls_max) polls_max = polls;
}

static void print_report(void)
{
    u32 k;
    u32 cycles = acct_cyc_idx_max - acct_cyc_idx_min;

    xil_printf("packets=%u min=%u max=%u(max@%u) avg=%u\r\n",
               pkt_count, bytes_min, bytes_max, bytes_max_at,
               pkt_count ? (bytes_sum / pkt_count) : 0U);
    xil_printf("hist(bytes<=");
    for (k = 0U; k < NBUCKETS; ++k) {
        xil_printf(" %u:%u", bucket_hi[k], hist[k]);
    }
    xil_printf(" over=%u zero=%u\r\n", hist_over, hist_zero);

    xil_printf("acct: peak=%u cyc=%u cycN=%u other=%u lastnotcyc=%u multicyc=%u\r\n",
               acct_peak_words, acct_cyc_words, acct_cyc_n_sum,
               acct_other_words, acct_last_not_cycle, acct_multi_cyc);
    if (cycles != 0U) {
        xil_printf("acct: cycles=%u pktsPerCycle_x100=%u evtsPerCycle=%u\r\n",
                   cycles,
                   (u32)(((u64)pkt_count * 100U) / cycles),
                   acct_cyc_n_sum / cycles);
    } else {
        xil_printf("acct: cycles=0 (no usable cycle_idx yet)\r\n");
    }
    xil_printf("acct: polls_min=%u polls_max=%u maxPktPolls=%u\r\n",
               polls_min, polls_max, bytes_max_polls);
}

int main(void)
{
    XAxiDma_Config *cfg;
    u32 transfer, s2mm_cr, s2mm_sr, s2mm_len, polls, actual;
    int status;

    xil_printf("\r\n--- S2MM stream-consistency probe v2 (BTT=%u) ---\r\n", S2MM_MAX_BTT);
    xil_printf("RX buffer: 0x%08x, BTT=%u bytes\r\n",
               (u32)RX_BUFFER_BASE, S2MM_MAX_BTT);

#if RX_NONCACHEABLE
    /* 0x01100000 is 1 MB aligned, so it maps as a non-cacheable section. */
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
    polls_min = 0xFFFFFFFFU;

    for (transfer = 0U; ; ++transfer) {
#if !RX_NONCACHEABLE
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
         * XAxiDma_Busy() is deliberately not used: it only looks at Idle and so
         * spins the full POLL_LIMIT after a graceful halt.
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
            if (s2mm_sr & DMASR_HALTED)      xil_printf("  Halted    : RS was cleared by the engine\r\n");
            if (s2mm_sr & DMASR_DMA_INT_ERR) xil_printf("  DMAIntErr : packet bigger than BTT, or memory-write error\r\n");
            if (s2mm_sr & DMASR_DMA_SLV_ERR) xil_printf("  DMASlvErr : slave error on the memory path\r\n");
            if (s2mm_sr & DMASR_DMA_DEC_ERR) xil_printf("  DMADecErr : decode error on the memory path\r\n");
            if (s2mm_sr & DMASR_ERR_IRQ)     xil_printf("  Err_Irq   : error interrupt raised for this transfer\r\n");
            if (s2mm_sr & DMASR_IOC_IRQ)     xil_printf("  IOC_Irq   : buffer filled before TLAST was seen\r\n");
            xil_printf("State before the failure:\r\n");
            print_report();
            dump_words(s2mm_len);
            xil_printf("Evidence preserved. Terminate the application now.\r\n");
            return -1;
        }

        if ((polls == POLL_LIMIT) && !(s2mm_sr & DMASR_IDLE)) {
            xil_printf("\r\n*** TIMEOUT - no error bits, no completion in %u polls ***\r\n",
                       POLL_LIMIT);
            xil_printf("TIMEOUT: transfer=%u DMACR=%08x DMASR=%08x LENGTH=%u\r\n",
                       transfer, s2mm_cr, s2mm_sr, s2mm_len);
            print_report();
            xil_printf("This is NOT the DMA error under investigation.\r\n");
            return -1;
        }

        account_packet(actual, polls);

        if (actual < bytes_min)          bytes_min = actual;
        if (actual > bytes_max) {
            bytes_max       = actual;
            bytes_max_at    = transfer;
            /* Poll count of the longest packet so far: an independent proxy for how
             * long the DMA waited, i.e. how much of the power cycle it spanned. */
            bytes_max_polls = polls;
        }
        bytes_sum += actual;
        hist_add(actual);
        pkt_count++;

        if ((transfer < DUMP_FIRST_N) ||
            ((actual > DUMP_LONG_BYTES) && (dump_long_count < DUMP_LONG_MAX))) {
            if (actual > DUMP_LONG_BYTES) {
                dump_long_count++;
            }
            xil_printf("transfer=%u bytes=%u beats=%u polls=%u DMASR=%08x\r\n",
                       transfer, actual, actual / 8U, polls, s2mm_sr);
            dump_words(actual);
        }

        if ((transfer % REPORT_PERIOD) == 0U) {
            xil_printf("completed=%u DMASR=%08x\r\n", pkt_count, s2mm_sr);
            print_report();
        }
    }
}
