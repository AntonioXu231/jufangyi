/*
 * Bounded AXI-DMA S2MM smoke test for pd_feature_0/m_axis.
 *
 * Purpose
 * -------
 * Prove the PS-side receive path after the automatic-snapshot test:
 *
 *   pd_feature_0/m_axis (64-bit AXI4-Stream)
 *        -> axi_dma_0 S2MM (Simple mode)
 *        -> PS DDR through HP0
 *        -> this program
 *
 * This is deliberately a bounded polling application.  It receives 128 AXI
 * Stream packets, verifies the DMA completion status and the event-stream
 * packet contract, prints a report, then parks in WFI.  It leaves no DMA
 * transfer armed, so it is safe to halt and download the next ELF.
 *
 * Hardware contract checked by this file
 * --------------------------------------
 * - AXI DMA: S2MM enabled, Simple mode, 64-bit stream, no Scatter-Gather.
 * - A packet ends on the cycle-statistics word (type 0x01 / TLAST).
 * - The DMA LENGTH register after completion is the number of bytes written.
 * - RX buffer 0x0110_0000 is outside code/stack and the raw DDR ring starting
 *   at 0x1000_2000.
 *
 * Usage in Vitis 2024.1 standalone
 * ---------------------------------
 * 1. Create/select an Empty Application using the current XSA platform.
 * 2. Replace src/main.c with this file.
 * 3. Build and launch with the already-programmed matching bitstream.
 * 4. Expected final result: FEATURE_DMA_S2MM_PASS.
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"

/* Vitis SDT uses base address as the driver lookup argument.  Keep the
 * legacy DeviceId branch so the source can also build with a classic BSP. */
#if defined(XPAR_XAXIDMA_0_BASEADDR)
# define DMA_BASE        ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG  XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
# define DMA_BASE        ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
# define DMA_LOOKUP_ARG  XPAR_AXIDMA_0_DEVICE_ID
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

/* Keep this below the application image and outside the raw ring / slot areas. */
#define RX_BUFFER_BASE       ((UINTPTR)PS_DDR_BASE + 0x01000000U)
#define S2MM_MAX_BTT         65528U  /* 16-bit BTT, aligned down to 8 bytes */
#define PACKET_LIMIT         128U
#define POLL_LIMIT           20000000U
#define REPORT_PERIOD        16U

/* AXI DMA PG021 S2MM register bank. */
#define S2MM_DMACR_OFFSET    0x30U
#define S2MM_DMASR_OFFSET    0x34U
#define S2MM_LENGTH_OFFSET   0x58U

#define DMASR_HALTED         0x00000001U
#define DMASR_IDLE           0x00000002U
#define DMASR_DMA_INT_ERR    0x00000010U
#define DMASR_DMA_SLV_ERR    0x00000020U
#define DMASR_DMA_DEC_ERR    0x00000040U
#define DMASR_IOC_IRQ        0x00001000U
#define DMASR_DLY_IRQ        0x00002000U
#define DMASR_ERR_IRQ        0x00004000U
#define DMASR_ERROR_MASK     (DMASR_DMA_INT_ERR | DMASR_DMA_SLV_ERR | \
                              DMASR_DMA_DEC_ERR | DMASR_ERR_IRQ)

#define EV_TYPE_PEAK         0x00U
#define EV_TYPE_CYCLE        0x01U

static XAxiDma g_dma;

static inline u32 dma_reg_read(u32 offset)
{
    return Xil_In32(DMA_BASE + offset);
}

static void park_after_failure(const char *reason)
{
    xil_printf("FEATURE_DMA_S2MM_FAIL: %s\r\n", reason);
    xil_printf("DMACR=%08x DMASR=%08x LENGTH=%u\r\n",
               dma_reg_read(S2MM_DMACR_OFFSET),
               dma_reg_read(S2MM_DMASR_OFFSET),
               dma_reg_read(S2MM_LENGTH_OFFSET));
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

static void print_event_word(u32 index, u64 word)
{
    u32 hi = (u32)(word >> 32);
    u32 lo = (u32)word;
    u32 type = (hi >> 24) & 0xffU;

    if (type == EV_TYPE_PEAK) {
        /* PD_PH_FIELD_W=12 build: phase[39:28], pol[27], ch[26:25]. */
        u32 phase = ((hi & 0xffU) << 4) | ((lo >> 28) & 0x0fU);
        u32 ch = (lo >> 25) & 0x3U;
        u32 pol = (lo >> 27) & 0x1U;
        s16 q = (s16)((hi >> 8) & 0xffffU);
        xil_printf("  %02u PEAK  raw=%08x_%08x q=%d phase=%u pol=%u ch=%u\r\n",
                   index, hi, lo, (s32)q, phase, pol, ch);
    } else if (type == EV_TYPE_CYCLE) {
        u32 cycle_idx = hi & 0x00ffffffU;
        u32 ch = (lo >> 30) & 0x3U;
        u32 event_count = (lo >> 16) & 0x3fffU;
        xil_printf("  %02u CYCLE raw=%08x_%08x idx=%u ch=%u n=%u qmax=%u\r\n",
                   index, hi, lo, cycle_idx, ch, event_count, lo & 0xffffU);
    } else {
        xil_printf("  %02u UNKNOWN raw=%08x_%08x type=%02x\r\n",
                   index, hi, lo, type);
    }
}

/* Every completed DMA transaction is one AXIS packet.  Verify that its last
 * beat is a cycle-statistics word, which is the RTL source of TLAST. */
static int inspect_packet(u32 packet_index, u32 bytes,
                          u32 *peak_words, u32 *cycle_words)
{
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
    u32 words = bytes / 8U;
    u32 i;
    u32 last_type;

    if ((bytes == 0U) || (bytes > S2MM_MAX_BTT) || ((bytes & 7U) != 0U))
        return XST_FAILURE;

    for (i = 0U; i < words; ++i) {
        u32 type = ((u32)(rx[i] >> 56)) & 0xffU;
        if (type == EV_TYPE_PEAK)
            (*peak_words)++;
        else if (type == EV_TYPE_CYCLE)
            (*cycle_words)++;
    }

    last_type = ((u32)(rx[words - 1U] >> 56)) & 0xffU;
    if (last_type != EV_TYPE_CYCLE)
        return XST_FAILURE;

    if ((packet_index < 2U) || ((packet_index % REPORT_PERIOD) == 0U)) {
        u32 show_words = (words < 8U) ? words : 8U;
        xil_printf("PKT %u: bytes=%u words=%u last_type=%02x\r\n",
                   packet_index, bytes, words, last_type);
        for (i = 0U; i < show_words; ++i)
            print_event_word(i, rx[i]);
        if (show_words < words)
            xil_printf("  ... (%u further words)\r\n", words - show_words);
    }
    return XST_SUCCESS;
}

int main(void)
{
    XAxiDma_Config *cfg;
    u32 packet;
    u32 peak_words = 0U;
    u32 cycle_words = 0U;
    u32 bytes_min = 0xffffffffU;
    u32 bytes_max = 0U;
    u32 bytes_total = 0U;
    int status;

    xil_printf("\r\n--- pd_feature AXI-DMA S2MM smoke test ---\r\n");
    xil_printf("DMA=0x%08x RX=0x%08x BTT=%u packets=%u\r\n",
               (u32)DMA_BASE, (u32)RX_BUFFER_BASE, S2MM_MAX_BTT, PACKET_LIMIT);

    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL)
        park_after_failure("XAxiDma_LookupConfig returned NULL");

    status = XAxiDma_CfgInitialize(&g_dma, cfg);
    if (status != XST_SUCCESS)
        park_after_failure("XAxiDma_CfgInitialize failed");
    if (XAxiDma_HasSg(&g_dma))
        park_after_failure("hardware is Scatter-Gather mode, expected Simple mode");

    XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&g_dma);
    while (!XAxiDma_ResetIsDone(&g_dma)) {
        /* Reset must complete before arming S2MM. */
    }

    for (packet = 0U; packet < PACKET_LIMIT; ++packet) {
        u32 i;
        u32 dmasr;
        u32 bytes;

        /* Avoid seeing a cache line from a preceding transaction. */
        Xil_DCacheFlushRange(RX_BUFFER_BASE, S2MM_MAX_BTT);
        Xil_DCacheInvalidateRange(RX_BUFFER_BASE, S2MM_MAX_BTT);
        XAxiDma_IntrAckIrq(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

        status = XAxiDma_SimpleTransfer(&g_dma, RX_BUFFER_BASE,
                                        S2MM_MAX_BTT, XAXIDMA_DEVICE_TO_DMA);
        if (status != XST_SUCCESS)
            park_after_failure("XAxiDma_SimpleTransfer failed");

        /* While S2MM is busy it drives AXIS TREADY.  A clean completion proves
         * that at least one TVALID/TREADY transfer reached PS DDR. */
        for (i = 0U; i < POLL_LIMIT; ++i) {
            if (!XAxiDma_Busy(&g_dma, XAXIDMA_DEVICE_TO_DMA))
                break;
        }

        dmasr = dma_reg_read(S2MM_DMASR_OFFSET);
        bytes = dma_reg_read(S2MM_LENGTH_OFFSET);
        if ((i == POLL_LIMIT) || (dmasr & DMASR_ERROR_MASK) ||
            (dmasr & DMASR_HALTED) || !(dmasr & DMASR_IDLE))
            park_after_failure("S2MM timeout, halt, or AXI error");

        if ((bytes == 0U) || (bytes > S2MM_MAX_BTT) || ((bytes & 7U) != 0U))
            park_after_failure("S2MM returned an invalid byte count");

        Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
        if (inspect_packet(packet, bytes, &peak_words, &cycle_words) != XST_SUCCESS)
            park_after_failure("event packet contract failed");

        if (bytes < bytes_min)
            bytes_min = bytes;
        if (bytes > bytes_max)
            bytes_max = bytes;
        bytes_total += bytes;
    }

    xil_printf("FEATURE_DMA_S2MM_PASS packets=%u peak_words=%u cycle_words=%u ",
               PACKET_LIMIT, peak_words, cycle_words);
    xil_printf("bytes_min=%u bytes_max=%u bytes_avg=%u\r\n",
               bytes_min, bytes_max, bytes_total / PACKET_LIMIT);
    xil_printf("STOP: DMA is idle; halt CPU before downloading another ELF.\r\n");
    for (;;) {
        __asm__ volatile ("wfi");
    }
    return 0;
}
