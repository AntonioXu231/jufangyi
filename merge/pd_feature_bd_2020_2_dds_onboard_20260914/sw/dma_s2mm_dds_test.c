/*
 * Minimal Zynq-7000 bare-metal receive test for the on-board DDS build.
 *
 * Verified hardware facts for this snapshot:
 *   AXI DMA base address : 0x4040_0000
 *   S2MM                : enabled, 64-bit AXI-Stream
 *   Scatter-Gather      : disabled (Simple mode)
 *   DDR HP0 aperture    : 0x0000_0000 .. 0x1fff_ffff
 *
 * Import this source into a Vitis standalone Cortex-A9 application.  Keep
 * the ILA-enabled bitstream programmed in Vivado; this application only
 * starts the PS and programs AXI DMA registers.
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "sleep.h"

/*
 * Vitis 2024 standalone uses System Device Tree (SDT) by default.  Its AXI
 * DMA driver identifies an instance by base address rather than DeviceId.
 * Keep the legacy branch for a future Vitis 2020.2 BSP build.
 */
#ifdef SDT
#ifndef XPAR_XAXIDMA_0_BASEADDR
#error "AXI DMA base-address macro is absent. Rebuild the Vitis platform from the current XSA."
#endif
#define DMA_LOOKUP_ARG   XPAR_XAXIDMA_0_BASEADDR
#else
#ifndef XPAR_AXIDMA_0_DEVICE_ID
#error "AXI DMA device-ID macro is absent. Rebuild the Vitis platform from the current XSA."
#endif
#define DMA_LOOKUP_ARG   XPAR_AXIDMA_0_DEVICE_ID
#endif

#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
#define PS_DDR_BASE       XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
#define PS_DDR_BASE       XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
#error "PS DDR base-address macro is absent. Inspect xparameters.h and update PS_DDR_BASE."
#endif

#define RX_BUFFER_BASE   (PS_DDR_BASE + 0x01000000U)
#define RX_WORDS         16U
#define RX_BYTES         (RX_WORDS * 8U)
#define POLL_LIMIT       200000000U
#define REPORT_PERIOD    1024U
#define DMA_PERIOD_US    1000U

/* AXI DMA S2MM register offsets and status bits (PG021, Simple mode). */
#define S2MM_DMACR_OFFSET 0x30U
#define S2MM_DMASR_OFFSET 0x34U
#define S2MM_LENGTH_OFFSET 0x58U
#define DMASR_HALTED      0x00000001U
#define DMASR_IDLE        0x00000002U
#define DMASR_DMA_INT_ERR 0x00000010U
#define DMASR_DMA_SLV_ERR 0x00000020U
#define DMASR_DMA_DEC_ERR 0x00000040U
#define DMASR_ERR_IRQ     0x00004000U
#define DMASR_ERROR_MASK  (DMASR_DMA_INT_ERR | DMASR_DMA_SLV_ERR | \
                          DMASR_DMA_DEC_ERR | DMASR_ERR_IRQ)

static XAxiDma AxiDma;

static void print_stream_word(u32 index, u64 event_word)
{
    const u32 hi        = (u32)(event_word >> 32);
    const u32 lo        = (u32)event_word;
    const u32 type      = (hi >> 24) & 0xffU;

    /* xil_printf supports %x/%d/%u reliably; do not use %l or 64-bit formats. */
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

int main(void)
{
    XAxiDma_Config *cfg;
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
    u32 i, transfer, s2mm_cr, s2mm_sr, s2mm_len;
    int status;

    xil_printf("\r\n--- DDS AXI-DMA S2MM receive test ---\r\n");
    xil_printf("RX buffer: 0x%08x, %u bytes\r\n", (u32)RX_BUFFER_BASE, RX_BYTES);

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
        xil_printf("ERROR: hardware is SG mode; this Simple-mode test must not run\r\n");
        return -1;
    }

    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&AxiDma);
    while (!XAxiDma_ResetIsDone(&AxiDma)) {
        /* Wait for the S2MM channel to leave reset. */
    }

    s2mm_cr = Xil_In32((UINTPTR)XPAR_XAXIDMA_0_BASEADDR + S2MM_DMACR_OFFSET);
    s2mm_sr = Xil_In32((UINTPTR)XPAR_XAXIDMA_0_BASEADDR + S2MM_DMASR_OFFSET);
    xil_printf("S2MM before arm: DMACR=%08x DMASR=%08x\r\n", s2mm_cr, s2mm_sr);
    xil_printf("Continuous S2MM mode: arm ILA now; terminate this app after capture.\r\n");

    for (transfer = 0U; ; ++transfer) {
        /* Prevent stale cache lines from being mistaken for received DMA data. */
        Xil_DCacheFlushRange((UINTPTR)RX_BUFFER_BASE, RX_BYTES);
        Xil_DCacheInvalidateRange((UINTPTR)RX_BUFFER_BASE, RX_BYTES);
        XAxiDma_IntrAckIrq(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

        status = XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)RX_BUFFER_BASE,
                                        RX_BYTES, XAXIDMA_DEVICE_TO_DMA);
        if (status != XST_SUCCESS) {
            xil_printf("ERROR: transfer=%u XAxiDma_SimpleTransfer=%d\r\n", transfer, status);
            return -1;
        }

        for (i = 0; i < POLL_LIMIT; ++i) {
            if (!XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA)) {
                break;
            }
        }
        s2mm_cr  = Xil_In32((UINTPTR)XPAR_XAXIDMA_0_BASEADDR + S2MM_DMACR_OFFSET);
        s2mm_sr  = Xil_In32((UINTPTR)XPAR_XAXIDMA_0_BASEADDR + S2MM_DMASR_OFFSET);
        s2mm_len = Xil_In32((UINTPTR)XPAR_XAXIDMA_0_BASEADDR + S2MM_LENGTH_OFFSET);

        if (i == POLL_LIMIT || (s2mm_sr & DMASR_ERROR_MASK) ||
            (s2mm_sr & DMASR_HALTED) || !(s2mm_sr & DMASR_IDLE)) {
            xil_printf("FAIL: transfer=%u DMACR=%08x DMASR=%08x LENGTH=%u polls=%u\r\n",
                       transfer, s2mm_cr, s2mm_sr, s2mm_len, i);
            return -1;
        }

        Xil_DCacheInvalidateRange((UINTPTR)RX_BUFFER_BASE, RX_BYTES);
        if (transfer == 0U) {
            xil_printf("First DMA completed. Received 16 stream words:\r\n");
            for (i = 0; i < RX_WORDS; ++i) {
                print_stream_word(i, rx[i]);
            }
        }
        if ((transfer % REPORT_PERIOD) == 0U) {
            xil_printf("S2MM running: completed=%u DMASR=%08x\r\n", transfer + 1U, s2mm_sr);
        }
        /* Leave an observable low-TREADY gap between captures (1 ms period). */
        usleep(DMA_PERIOD_US);
    }
}
