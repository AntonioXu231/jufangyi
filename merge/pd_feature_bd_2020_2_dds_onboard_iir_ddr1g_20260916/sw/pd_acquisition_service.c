/*
 * pd_acquisition_service.c -- standalone acquisition service V1
 *
 * This is the first long-running PS-side acquisition application for the
 * current Zynq-7020 DDS prototype.  It deliberately separates ownership:
 *
 *   AXIS feature packet -> RX buffer -> event archive ring (16 x 64 KiB)
 *   raw DDR snapshot   -> slot lock -> snapshot archive ring (4 x 12 MiB)
 *
 * Each archived item has a metadata entry in g_acq.  A future Ethernet/UART
 * host protocol can read that structure and the physical archive ranges
 * without changing the PL capture contract.  Until that protocol exists,
 * both archive rings retain the newest records and overwrite the oldest;
 * overwrites are explicit in event_overwrites/snapshot_overwrites.
 *
 * This source intentionally does not write pd_filter coefficient registers or
 * pd_feature thresholds.  Those are calibration-owned settings.  Run the
 * separately validated pd_filter_apply program before this service only when
 * its coefficients have been approved.
 *
 * The UART command START 128 performs a bounded run; START 0 runs continuously
 * until STOP.  PD_ACQ_PACKET_LIMIT is only the default used by a bare START.
 */

#include "xparameters.h"
#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"
#include "xuartps_hw.h"
#include <string.h>

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
# error "No AXI-DMA base macro: regenerate the Vitis platform from the matching XSA."
#endif

#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
# define PS_DDR_BASE XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
# define PS_DDR_BASE XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
# error "No PS DDR base macro in xparameters.h"
#endif

/* pd_ddr AXI-Lite register map. */
#define DDR_CTRL                 0x000U
#define DDR_STATUS               0x004U
#define FREEZE_CTRL              0x034U
#define SLOT_CTRL                0x04CU
#define SLOT_STATUS              0x050U
#define SLOT_SEQ                 0x054U
#define SLOT_DROPS               0x058U
#define SNAP_TRIG_CTRL           0x05CU
#define SLOT_BASE_OFF(n)         (0x060U + ((n) * 0x10U))
#define SLOT_LEN_OFF(n)          (0x064U + ((n) * 0x10U))
#define SLOT_SEQ_OFF(n)          (0x068U + ((n) * 0x10U))
#define SLOT_FLAGS_OFF(n)        (0x06CU + ((n) * 0x10U))

#define DDR_COPY_BUSY             (1U << 1)
#define DDR_RING_ERR              (1U << 5)
#define DDR_COPY_ERR              (1U << 8)
#define DDR_CFG_ERR               (1U << 9)
#define SLOT_VALID_MASK           0x0000000FU
#define SLOT_BUSY_MASK            0x000000F0U
#define SLOT_LOCKED_MASK          0x00000F00U
#define SLOT_CFG_ERR              (1U << 13)
#define SLOT_REQ_OVERFLOW         (1U << 14)
#define SLOT_CMD_ERR              (1U << 15)
#define SLOT_LAST_SHIFT           17U
#define SLOT_READY                (1U << 19)
#define TRIG_ENABLED              (1U << 0)
#define TRIG_MASK_ALL             (0xFU << 1)
#define TRIG_ARMED                (1U << 16)

/* AXI DMA S2MM register bank (PG021). */
#define S2MM_DMACR_OFFSET         0x30U
#define S2MM_DMASR_OFFSET         0x34U
#define S2MM_LENGTH_OFFSET        0x58U
#define DMASR_HALTED              0x00000001U
#define DMASR_IDLE                0x00000002U
#define DMASR_ERROR_MASK          0x00004070U

/* DDR layout: these ranges are after the PL raw ring (0x10002000..0x17ffffff)
 * and hardware slot area (0x20001000..0x23000fff), and below 1 GiB PS DDR. */
#define RX_BUFFER_BASE            ((UINTPTR)PS_DDR_BASE + 0x01000000U)
#define RX_BUFFER_BYTES           65528U
#define EVENT_ARCHIVE_BASE        0x27000000U
#define EVENT_ARCHIVE_COUNT       16U
#define EVENT_ARCHIVE_STRIDE      0x00010000U
#define SNAP_ARCHIVE_BASE         0x24000000U
#define SNAP_ARCHIVE_COUNT        4U
#define SNAP_ARCHIVE_STRIDE       0x00C00000U
#define SNAP_SLOT_LOW             0x20001000U
#define SNAP_SLOT_HIGH            0x23001000U

/* Set to 0U only after the bounded first board run has passed. */
#ifndef PD_ACQ_PACKET_LIMIT
# define PD_ACQ_PACKET_LIMIT      128U
#endif
#define DMA_POLL_LIMIT            20000000U
#define SHUTDOWN_POLL_LIMIT       1000000U
#define REPORT_PERIOD             32U
#define UART_BASE                 XPAR_XUARTPS_0_BASEADDR
#define UART_LINE_BYTES           48U

typedef struct {
    u32 sequence;
    u32 ddr_addr;
    u32 bytes;
    u32 peak_words;
    u32 cycle_words;
} pd_event_record_t;

typedef struct {
    u32 sequence;
    u32 source_slot;
    u32 source_addr;
    u32 archive_addr;
    u32 bytes;
    u32 hw_slot_sequence;
    u32 flags;
} pd_snapshot_record_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 event_sequence;
    u32 snapshot_sequence;
    u32 event_overwrites;
    u32 snapshot_overwrites;
    u32 dma_errors;
    u32 slot_errors;
    u32 last_dma_status;
    u32 last_ddr_status;
    u32 last_slot_status;
    pd_event_record_t event[EVENT_ARCHIVE_COUNT];
    pd_snapshot_record_t snapshot[SNAP_ARCHIVE_COUNT];
} pd_acq_shared_t;

/* Keep this symbol global: an eventual host protocol can publish its address
 * or copy it to a transport response without changing capture code. */
volatile pd_acq_shared_t g_acq;
static XAxiDma g_dma;
static volatile u32 g_running;
static volatile u32 g_stop_request;
static volatile u32 g_quit_request;
static volatile u32 g_start_request;
static volatile u32 g_start_limit;
static char g_uart_line[UART_LINE_BYTES];
static u32 g_uart_len;

static u32 ddr_read(u32 off) { return Xil_In32(PD_DDR_BASE + off); }
static void ddr_write(u32 off, u32 value) { Xil_Out32(PD_DDR_BASE + off, value); }
static u32 dma_read(u32 off) { return Xil_In32(DMA_BASE + off); }

static void print_status(void)
{
    xil_printf("STATUS run=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u ",
               g_running, g_acq.event_sequence, g_acq.snapshot_sequence,
               g_acq.event_overwrites, g_acq.snapshot_overwrites);
    xil_printf("ddr=%08x slot=%08x dma=%08x drops=%u\r\n",
               ddr_read(DDR_STATUS), ddr_read(SLOT_STATUS),
               dma_read(S2MM_DMASR_OFFSET), ddr_read(SLOT_DROPS));
}

static u32 parse_u32(const char *p, u32 default_value)
{
    u32 value = 0U;
    u32 any = 0U;
    while (*p == ' ') ++p;
    while (*p >= '0' && *p <= '9') {
        any = 1U;
        value = value * 10U + (u32)(*p - '0');
        ++p;
    }
    return any ? value : default_value;
}

static void print_event_record(u32 index)
{
    if (index >= EVENT_ARCHIVE_COUNT || g_acq.event[index].sequence >= g_acq.event_sequence) {
        xil_printf("ERR EVENT index has no valid record\r\n");
        return;
    }
    xil_printf("EVENT index=%u seq=%u addr=%08x bytes=%u peaks=%u cycles=%u\r\n",
               index, g_acq.event[index].sequence, g_acq.event[index].ddr_addr,
               g_acq.event[index].bytes, g_acq.event[index].peak_words,
               g_acq.event[index].cycle_words);
}

static void print_snapshot_record(u32 index)
{
    if (index >= SNAP_ARCHIVE_COUNT || g_acq.snapshot[index].sequence >= g_acq.snapshot_sequence) {
        xil_printf("ERR SNAP index has no valid record\r\n");
        return;
    }
    xil_printf("SNAP index=%u seq=%u hw_slot=%u src=%08x dst=%08x bytes=%u flags=%08x\r\n",
               index, g_acq.snapshot[index].sequence, g_acq.snapshot[index].source_slot,
               g_acq.snapshot[index].source_addr, g_acq.snapshot[index].archive_addr,
               g_acq.snapshot[index].bytes, g_acq.snapshot[index].flags);
}

static void clear_metadata(void)
{
    if (g_running) {
        xil_printf("ERR CLEAR is accepted only while idle\r\n");
        return;
    }
    memset((void *)&g_acq, 0, sizeof(g_acq));
    g_acq.magic = 0x50444151U;
    g_acq.version = 1U;
    xil_printf("OK CLEAR metadata\r\n");
}

static void execute_command(char *line)
{
    if (strcmp(line, "HELP") == 0) {
        xil_printf("CMD: START [packets] | STOP | STATUS | EVENT n | SNAP n | CLEAR | QUIT\r\n");
    } else if (strncmp(line, "START", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        if (g_running) xil_printf("ERR already running\r\n");
        else { g_start_limit = parse_u32(line + 5, PD_ACQ_PACKET_LIMIT); g_start_request = 1U; }
    } else if (strcmp(line, "STOP") == 0) {
        if (!g_running) xil_printf("OK already idle\r\n");
        else { g_stop_request = 1U; xil_printf("OK stop requested\r\n"); }
    } else if (strcmp(line, "STATUS") == 0) {
        print_status();
    } else if (strncmp(line, "EVENT ", 6) == 0) {
        print_event_record(parse_u32(line + 6, EVENT_ARCHIVE_COUNT));
    } else if (strncmp(line, "SNAP ", 5) == 0) {
        print_snapshot_record(parse_u32(line + 5, SNAP_ARCHIVE_COUNT));
    } else if (strcmp(line, "CLEAR") == 0) {
        clear_metadata();
    } else if (strcmp(line, "QUIT") == 0) {
        if (g_running) { g_stop_request = 1U; g_quit_request = 1U; xil_printf("OK stop then quit\r\n"); }
        else g_quit_request = 1U;
    } else if (line[0] != 0) {
        xil_printf("ERR unknown command; type HELP\r\n");
    }
}

/* Polling is deliberate here: it avoids a second interrupt source while this
 * command protocol is being brought up.  It is called inside DMA waits too. */
static void uart_poll(void)
{
    while (XUartPs_IsReceiveData(UART_BASE)) {
        char c = (char)XUartPs_RecvByte(UART_BASE);
        if (c == '\r' || c == '\n') {
            if (g_uart_len != 0U) {
                g_uart_line[g_uart_len] = 0;
                execute_command(g_uart_line);
                g_uart_len = 0U;
            }
        } else if (c == 8 || c == 127) {
            if (g_uart_len != 0U) --g_uart_len;
        } else if (c >= ' ' && c <= '~') {
            if (g_uart_len + 1U < UART_LINE_BYTES) g_uart_line[g_uart_len++] = c;
            else { g_uart_len = 0U; xil_printf("ERR command too long\r\n"); }
        }
    }
}

static void halt_failure(const char *why)
{
    g_acq.last_ddr_status = ddr_read(DDR_STATUS);
    g_acq.last_slot_status = ddr_read(SLOT_STATUS);
    g_acq.last_dma_status = dma_read(S2MM_DMASR_OFFSET);
    xil_printf("ACQ_SERVICE_FAIL: %s ddr=%08x slot=%08x dma=%08x\r\n", why,
               g_acq.last_ddr_status, g_acq.last_slot_status, g_acq.last_dma_status);
    for (;;) __asm__ volatile ("wfi");
}

static void configure_capture(void)
{
    u32 i;
    ddr_write(DDR_CTRL, 0U);
    ddr_write(FREEZE_CTRL, 2U);
    for (i = 0U; i < SHUTDOWN_POLL_LIMIT; ++i) {
        if ((ddr_read(DDR_STATUS) & (1U | DDR_RING_ERR)) == 0U)
            break;
    }
    if (i == SHUTDOWN_POLL_LIMIT) halt_failure("stale ring status did not clear");

    ddr_write(SLOT_CTRL, (1U << 12));
    ddr_write(SNAP_TRIG_CTRL, (1U << 8) | TRIG_MASK_ALL | TRIG_ENABLED);
    ddr_write(DDR_CTRL, 1U);
    ddr_write(SLOT_CTRL, 1U);
    ddr_write(SNAP_TRIG_CTRL, TRIG_MASK_ALL | TRIG_ENABLED);
    if ((ddr_read(SNAP_TRIG_CTRL) & (TRIG_ENABLED | TRIG_ARMED)) !=
        (TRIG_ENABLED | TRIG_ARMED))
        halt_failure("automatic snapshot trigger did not arm");
}

static void archive_event_packet(u32 bytes)
{
    u32 i, peaks = 0U, cycles = 0U, words = bytes / 8U;
    u32 seq = g_acq.event_sequence;
    u32 index = seq % EVENT_ARCHIVE_COUNT;
    UINTPTR dst = EVENT_ARCHIVE_BASE + index * EVENT_ARCHIVE_STRIDE;
    volatile u64 *rx = (volatile u64 *)RX_BUFFER_BASE;
    u32 last_type;

    if (!bytes || bytes > RX_BUFFER_BYTES || (bytes & 7U)) halt_failure("bad DMA byte count");
    for (i = 0U; i < words; ++i) {
        u32 type = (u32)(rx[i] >> 56);
        if (type == 0U) ++peaks;
        else if (type == 1U) ++cycles;
        else halt_failure("unknown event word type");
    }
    last_type = (u32)(rx[words - 1U] >> 56);
    if (last_type != 1U) halt_failure("AXIS packet did not end in cycle word");

    memcpy((void *)dst, (const void *)RX_BUFFER_BASE, bytes);
    Xil_DCacheFlushRange(dst, bytes);
    if (seq >= EVENT_ARCHIVE_COUNT) ++g_acq.event_overwrites;
    g_acq.event[index].sequence = seq;
    g_acq.event[index].ddr_addr = (u32)dst;
    g_acq.event[index].bytes = bytes;
    g_acq.event[index].peak_words = peaks;
    g_acq.event[index].cycle_words = cycles;
    ++g_acq.event_sequence;
}

/* Return zero only for a host STOP observed while the DMA is waiting for a
 * packet.  Resetting S2MM deliberately discards that partial packet. */
static int receive_one_packet(void)
{
    u32 i, bytes, dmasr;
    Xil_DCacheFlushRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_BUFFER_BYTES);
    XAxiDma_IntrAckIrq(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    if (XAxiDma_SimpleTransfer(&g_dma, RX_BUFFER_BASE, RX_BUFFER_BYTES,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS)
        halt_failure("DMA submit failed");
    for (i = 0U; i < DMA_POLL_LIMIT; ++i) {
        uart_poll();
        if (g_stop_request) {
            XAxiDma_Reset(&g_dma);
            while (!XAxiDma_ResetIsDone(&g_dma)) { }
            return 0;
        }
        if (!XAxiDma_Busy(&g_dma, XAXIDMA_DEVICE_TO_DMA)) break;
    }
    dmasr = dma_read(S2MM_DMASR_OFFSET);
    bytes = dma_read(S2MM_LENGTH_OFFSET);
    g_acq.last_dma_status = dmasr;
    if (i == DMA_POLL_LIMIT || (dmasr & (DMASR_HALTED | DMASR_ERROR_MASK)) ||
        !(dmasr & DMASR_IDLE)) {
        ++g_acq.dma_errors;
        halt_failure("DMA completion failed");
    }
    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, bytes);
    archive_event_packet(bytes);
    return 1;
}

static void archive_new_snapshot(u32 *last_slot_sequence)
{
    u32 status = ddr_read(SLOT_STATUS);
    u32 seq = ddr_read(SLOT_SEQ);
    u32 slot, base, bytes, slot_seq, flags, index;
    UINTPTR dst;

    g_acq.last_ddr_status = ddr_read(DDR_STATUS);
    g_acq.last_slot_status = status;
    if (status & (SLOT_CFG_ERR | SLOT_REQ_OVERFLOW | SLOT_CMD_ERR)) {
        ++g_acq.slot_errors;
        halt_failure("slot manager error");
    }
    if (g_acq.last_ddr_status & (DDR_COPY_ERR | DDR_CFG_ERR)) halt_failure("DDR copy error");
    if (!(status & SLOT_READY) || seq == *last_slot_sequence) return;

    slot = (status >> SLOT_LAST_SHIFT) & 0x3U;
    if (!(status & (1U << slot))) halt_failure("READY without valid slot");
    ddr_write(SLOT_CTRL, 1U << (4U + slot));
    if (!(ddr_read(SLOT_STATUS) & (1U << (8U + slot)))) halt_failure("slot lock failed");

    base = ddr_read(SLOT_BASE_OFF(slot));
    bytes = ddr_read(SLOT_LEN_OFF(slot));
    slot_seq = ddr_read(SLOT_SEQ_OFF(slot));
    flags = ddr_read(SLOT_FLAGS_OFF(slot));
    if ((base & 7U) || (bytes & 7U) || !bytes || bytes > SNAP_ARCHIVE_STRIDE ||
        base < SNAP_SLOT_LOW || base + bytes > SNAP_SLOT_HIGH)
        halt_failure("invalid snapshot descriptor");

    index = g_acq.snapshot_sequence % SNAP_ARCHIVE_COUNT;
    dst = SNAP_ARCHIVE_BASE + index * SNAP_ARCHIVE_STRIDE;
    Xil_DCacheInvalidateRange((UINTPTR)base, bytes);
    memcpy((void *)dst, (const void *)(UINTPTR)base, bytes);
    Xil_DCacheFlushRange(dst, bytes);
    if (g_acq.snapshot_sequence >= SNAP_ARCHIVE_COUNT) ++g_acq.snapshot_overwrites;
    g_acq.snapshot[index].sequence = g_acq.snapshot_sequence;
    g_acq.snapshot[index].source_slot = slot;
    g_acq.snapshot[index].source_addr = base;
    g_acq.snapshot[index].archive_addr = (u32)dst;
    g_acq.snapshot[index].bytes = bytes;
    g_acq.snapshot[index].hw_slot_sequence = slot_seq;
    g_acq.snapshot[index].flags = flags;
    ++g_acq.snapshot_sequence;

    ddr_write(SLOT_CTRL, 1U << (8U + slot));
    if (ddr_read(SLOT_STATUS) & (1U << (8U + slot))) halt_failure("slot release failed");
    ddr_write(FREEZE_CTRL, 2U);
    *last_slot_sequence = seq;
    xil_printf("SNAP_ARCH seq=%u slot=%u src=%08x dst=%08x bytes=%u\r\n",
               g_acq.snapshot_sequence - 1U, slot, base, (u32)dst, bytes);
}

/* Stop admission first, then drain a trigger that was already accepted before
 * that write reached PL.  A clean DMA/slot-busy state alone is insufficient:
 * a completed-but-unarchived slot is valid data and must be copied/released. */
static void clean_stop(u32 *last_slot_sequence)
{
    u32 i;
    u32 snapshots_before = g_acq.snapshot_sequence;
    ddr_write(SNAP_TRIG_CTRL, 0U);
    ddr_write(DDR_CTRL, 0U);
    ddr_write(FREEZE_CTRL, 2U);
    for (i = 0U; i < SHUTDOWN_POLL_LIMIT; ++i) {
        u32 ddr = ddr_read(DDR_STATUS);
        u32 slot = ddr_read(SLOT_STATUS);
        archive_new_snapshot(last_slot_sequence);
        ddr = ddr_read(DDR_STATUS);
        slot = ddr_read(SLOT_STATUS);
        if (!(ddr & DDR_COPY_BUSY) && !(slot & SLOT_BUSY_MASK) &&
            !(slot & SLOT_VALID_MASK)) {
            xil_printf("ACQ_CLEAN_STOP ddr=%08x slot=%08x polls=%u drained=%u\r\n",
                       ddr, slot, i, g_acq.snapshot_sequence - snapshots_before);
            return;
        }
    }
    halt_failure("capture did not quiesce");
}

int main(void)
{
    XAxiDma_Config *cfg;
    u32 packets = 0U, last_slot_sequence;

    memset((void *)&g_acq, 0, sizeof(g_acq));
    g_acq.magic = 0x50444151U; /* "PDAQ" */
    g_acq.version = 1U;
    xil_printf("\r\n--- pd acquisition UART service V1 ---\r\n");
    xil_printf("event archive=%08x x%u, snapshot archive=%08x x%u\r\n",
               EVENT_ARCHIVE_BASE, EVENT_ARCHIVE_COUNT, SNAP_ARCHIVE_BASE,
               SNAP_ARCHIVE_COUNT);

    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL) halt_failure("DMA config not found");
    if (XAxiDma_CfgInitialize(&g_dma, cfg) != XST_SUCCESS) halt_failure("DMA init failed");
    if (XAxiDma_HasSg(&g_dma)) halt_failure("expected Simple-mode DMA");
    XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&g_dma);
    while (!XAxiDma_ResetIsDone(&g_dma)) { }

    xil_printf("READY: type HELP, then START 128 (or START 0 for continuous)\r\n");
    while (!g_quit_request) {
        uart_poll();

        if (!g_running) {
            if (!g_start_request) continue;
            g_start_request = 0U;
            g_stop_request = 0U;
            packets = 0U;
            configure_capture();
            last_slot_sequence = ddr_read(SLOT_SEQ);
            g_running = 1U;
            xil_printf("OK START limit=%u seq=%u\r\n", g_start_limit, last_slot_sequence);
            continue;
        }

        if (g_stop_request || !receive_one_packet()) {
            archive_new_snapshot(&last_slot_sequence);
            clean_stop(&last_slot_sequence);
            g_running = 0U;
            g_stop_request = 0U;
            xil_printf("OK STOP packets=%u events=%u snapshots=%u\r\n",
                       packets, g_acq.event_sequence, g_acq.snapshot_sequence);
            continue;
        }

        archive_new_snapshot(&last_slot_sequence);
        ++packets;
        if ((packets % REPORT_PERIOD) == 0U)
            xil_printf("ACQ packets=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u drops=%u\r\n",
                       packets, g_acq.event_sequence, g_acq.snapshot_sequence,
                       g_acq.event_overwrites, g_acq.snapshot_overwrites,
                       ddr_read(SLOT_DROPS));
        if (g_start_limit != 0U && packets >= g_start_limit) {
            archive_new_snapshot(&last_slot_sequence);
            clean_stop(&last_slot_sequence);
            g_running = 0U;
            xil_printf("ACQ_SERVICE_PASS packets=%u events=%u snapshots=%u ev_ovw=%u snap_ovw=%u\r\n",
                       packets, g_acq.event_sequence, g_acq.snapshot_sequence,
                       g_acq.event_overwrites, g_acq.snapshot_overwrites);
        }
    }

    xil_printf("BYE: service is quiescent.\r\n");
    for (;;) __asm__ volatile ("wfi");
    return 0;
}
