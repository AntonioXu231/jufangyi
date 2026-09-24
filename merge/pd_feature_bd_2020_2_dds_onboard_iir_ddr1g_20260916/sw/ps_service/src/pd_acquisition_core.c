/*
 * pd_acquisition_core.c -- non-blocking DMA/DDR capture core for PS-1.
 *
 * This file owns DMA and snapshot-slot state.  UART/TCP/UDP front ends must
 * use its public API instead of accessing AXI registers or g_pd_acq directly.
 */
#include "pd_acquisition.h"
#include "pd_hw_map.h"

#include "xaxidma.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"
#include <string.h>

#define PD_DMA_POLL_LIMIT       20000000U
#define PD_SHUTDOWN_POLL_LIMIT  1000000U
/*
 * SLOT_STATUS is sampled through AXI-Lite while the slot manager is updating
 * it in the PL clock domain.  READY/sequence may therefore be observed one
 * software poll before the selected VALID bit.  This is a retry limit, not a
 * new hardware timeout: a durable mismatch remains a capture fault.
 */
#define PD_SLOT_READY_POLL_LIMIT 1000000U

volatile pd_acq_shared_t g_pd_acq;

static XAxiDma s_dma;
static pd_acq_state_t s_state = PD_ACQ_IDLE;
static pd_acq_poll_hook_t s_poll_hook;
static u32 s_packet_limit;
static u32 s_default_packet_limit = 128U;
static u32 s_packets;
static u32 s_last_slot_sequence;
static u32 s_dma_inflight;
static u32 s_dma_polls;
static u32 s_slot_ready_polls;
static const char *s_last_error;

static u32 ddr_read(u32 off) { return Xil_In32(PD_DDR_BASE + off); }
static void ddr_write(u32 off, u32 value) { Xil_Out32(PD_DDR_BASE + off, value); }
static u32 dma_read(u32 off) { return Xil_In32(PD_DMA_BASE + off); }

static int fail(const char *why)
{
    s_last_error = why;
    g_pd_acq.last_ddr_status = ddr_read(PD_DDR_STATUS);
    g_pd_acq.last_slot_status = ddr_read(PD_SLOT_STATUS);
    g_pd_acq.last_dma_status = dma_read(PD_S2MM_DMASR_OFFSET);
    s_state = PD_ACQ_FAULT;
    xil_printf("PD_ACQ_FAULT: %s ddr=%08x slot=%08x dma=%08x\r\n", why,
               g_pd_acq.last_ddr_status, g_pd_acq.last_slot_status,
               g_pd_acq.last_dma_status);
    return XST_FAILURE;
}

static int configure_capture(void)
{
    u32 i;
    ddr_write(PD_DDR_CTRL, 0U);
    ddr_write(PD_FREEZE_CTRL, 2U);
    for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i) {
        if ((ddr_read(PD_DDR_STATUS) & (1U | PD_DDR_RING_ERR)) == 0U) break;
        if (s_poll_hook != NULL) s_poll_hook();
    }
    if (i == PD_SHUTDOWN_POLL_LIMIT) return fail("stale ring status did not clear");

    ddr_write(PD_SLOT_CTRL, (1U << 12));
    ddr_write(PD_SNAP_TRIG_CTRL, (1U << 8) | PD_TRIG_MASK_ALL | PD_TRIG_ENABLED);
    ddr_write(PD_DDR_CTRL, 1U);
    ddr_write(PD_SLOT_CTRL, 1U);
    ddr_write(PD_SNAP_TRIG_CTRL, PD_TRIG_MASK_ALL | PD_TRIG_ENABLED);
    if ((ddr_read(PD_SNAP_TRIG_CTRL) & (PD_TRIG_ENABLED | PD_TRIG_ARMED)) !=
        (PD_TRIG_ENABLED | PD_TRIG_ARMED)) return fail("automatic snapshot trigger did not arm");
    return XST_SUCCESS;
}

static int archive_event_packet(u32 bytes)
{
    u32 i, peaks = 0U, cycles = 0U, words = bytes / 8U;
    u32 seq = g_pd_acq.event_sequence;
    u32 index = seq % PD_EVENT_ARCHIVE_COUNT;
    UINTPTR dst = PD_EVENT_ARCHIVE_BASE + index * PD_EVENT_ARCHIVE_STRIDE;
    volatile u64 *rx = (volatile u64 *)PD_RX_BUFFER_BASE;

    if (!bytes || bytes > PD_RX_BUFFER_BYTES || (bytes & 7U)) return fail("bad DMA byte count");
    for (i = 0U; i < words; ++i) {
        u32 type = (u32)(rx[i] >> 56);
        if (type == 0U) ++peaks;
        else if (type == 1U) ++cycles;
        else return fail("unknown event word type");
    }
    if ((u32)(rx[words - 1U] >> 56) != 1U) return fail("AXIS packet did not end in cycle word");

    memcpy((void *)dst, (const void *)PD_RX_BUFFER_BASE, bytes);
    Xil_DCacheFlushRange(dst, bytes);
    if (seq >= PD_EVENT_ARCHIVE_COUNT) ++g_pd_acq.event_overwrites;
    g_pd_acq.event[index].sequence = seq;
    g_pd_acq.event[index].ddr_addr = (u32)dst;
    g_pd_acq.event[index].bytes = bytes;
    g_pd_acq.event[index].peak_words = peaks;
    g_pd_acq.event[index].cycle_words = cycles;
    ++g_pd_acq.event_sequence;
    return XST_SUCCESS;
}

static int archive_new_snapshot(void)
{
    u32 status = ddr_read(PD_SLOT_STATUS);
    u32 seq = ddr_read(PD_SLOT_SEQ);
    u32 slot, base, bytes, slot_seq, flags, index;
    UINTPTR dst;

    g_pd_acq.last_ddr_status = ddr_read(PD_DDR_STATUS);
    g_pd_acq.last_slot_status = status;
    if (status & (PD_SLOT_CFG_ERR | PD_SLOT_REQ_OVERFLOW | PD_SLOT_CMD_ERR)) {
        ++g_pd_acq.slot_errors;
        return fail("slot manager error");
    }
    if (g_pd_acq.last_ddr_status & (PD_DDR_COPY_ERR | PD_DDR_CFG_ERR))
        return fail("DDR copy error");
    if (!(status & PD_SLOT_READY) || seq == s_last_slot_sequence) return XST_SUCCESS;

    slot = (status >> PD_SLOT_LAST_SHIFT) & 0x3U;
    if (!(status & (1U << slot))) {
        /* Do not consume seq: retry until this slot descriptor is coherent. */
        if (++s_slot_ready_polls < PD_SLOT_READY_POLL_LIMIT) return XST_SUCCESS;
        return fail("READY without valid slot timeout");
    }
    s_slot_ready_polls = 0U;
    ddr_write(PD_SLOT_CTRL, 1U << (4U + slot));
    if (!(ddr_read(PD_SLOT_STATUS) & (1U << (8U + slot)))) return fail("slot lock failed");

    base = ddr_read(PD_SLOT_BASE_OFF(slot));
    bytes = ddr_read(PD_SLOT_LEN_OFF(slot));
    slot_seq = ddr_read(PD_SLOT_SEQ_OFF(slot));
    flags = ddr_read(PD_SLOT_FLAGS_OFF(slot));
    if ((base & 7U) || (bytes & 7U) || !bytes || bytes > PD_SNAP_ARCHIVE_STRIDE ||
        base < PD_SNAP_SLOT_LOW || base + bytes > PD_SNAP_SLOT_HIGH)
        return fail("invalid snapshot descriptor");

    index = g_pd_acq.snapshot_sequence % PD_SNAP_ARCHIVE_COUNT;
    dst = PD_SNAP_ARCHIVE_BASE + index * PD_SNAP_ARCHIVE_STRIDE;
    Xil_DCacheInvalidateRange((UINTPTR)base, bytes);
    memcpy((void *)dst, (const void *)(UINTPTR)base, bytes);
    Xil_DCacheFlushRange(dst, bytes);
    if (g_pd_acq.snapshot_sequence >= PD_SNAP_ARCHIVE_COUNT) ++g_pd_acq.snapshot_overwrites;
    g_pd_acq.snapshot[index].sequence = g_pd_acq.snapshot_sequence;
    g_pd_acq.snapshot[index].source_slot = slot;
    g_pd_acq.snapshot[index].source_addr = base;
    g_pd_acq.snapshot[index].archive_addr = (u32)dst;
    g_pd_acq.snapshot[index].bytes = bytes;
    g_pd_acq.snapshot[index].hw_slot_sequence = slot_seq;
    g_pd_acq.snapshot[index].flags = flags;
    ++g_pd_acq.snapshot_sequence;

    ddr_write(PD_SLOT_CTRL, 1U << (8U + slot));
    if (ddr_read(PD_SLOT_STATUS) & (1U << (8U + slot))) return fail("slot release failed");
    ddr_write(PD_FREEZE_CTRL, 2U);
    s_last_slot_sequence = seq;
    xil_printf("SNAP_ARCH seq=%u slot=%u src=%08x dst=%08x bytes=%u\r\n",
               g_pd_acq.snapshot_sequence - 1U, slot, base, (u32)dst, bytes);
    return XST_SUCCESS;
}

static int submit_dma(void)
{
    Xil_DCacheFlushRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES);
    Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES);
    XAxiDma_IntrAckIrq(&s_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    if (XAxiDma_SimpleTransfer(&s_dma, PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS)
        return fail("DMA submit failed");
    s_dma_inflight = 1U;
    s_dma_polls = 0U;
    return XST_SUCCESS;
}

static int reset_dma(void)
{
    u32 i;
    XAxiDma_Reset(&s_dma);
    for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i)
        if (XAxiDma_ResetIsDone(&s_dma)) return XST_SUCCESS;
    return fail("DMA reset timeout");
}

static int stop_and_drain(void)
{
    u32 i;
    ddr_write(PD_SNAP_TRIG_CTRL, 0U);
    ddr_write(PD_DDR_CTRL, 0U);
    ddr_write(PD_FREEZE_CTRL, 2U);
    for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i) {
        if (archive_new_snapshot() != XST_SUCCESS) return XST_FAILURE;
        if (!(ddr_read(PD_DDR_STATUS) & PD_DDR_COPY_BUSY) &&
            !(ddr_read(PD_SLOT_STATUS) & (PD_SLOT_BUSY_MASK | PD_SLOT_VALID_MASK))) {
            s_state = PD_ACQ_IDLE;
            return XST_SUCCESS;
        }
        if (s_poll_hook != NULL) s_poll_hook();
    }
    return fail("capture did not quiesce");
}

int pd_acq_init(void)
{
    XAxiDma_Config *cfg;
    memset((void *)&g_pd_acq, 0, sizeof(g_pd_acq));
    g_pd_acq.magic = 0x50444151U; /* PDAQ */
    g_pd_acq.version = 3U;
    cfg = XAxiDma_LookupConfig(PD_DMA_LOOKUP_ARG);
    if (cfg == NULL) return fail("DMA config not found");
    if (XAxiDma_CfgInitialize(&s_dma, cfg) != XST_SUCCESS) return fail("DMA init failed");
    if (XAxiDma_HasSg(&s_dma)) return fail("expected Simple-mode DMA");
    XAxiDma_IntrDisable(&s_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&s_dma);
    while (!XAxiDma_ResetIsDone(&s_dma)) { }
    s_state = PD_ACQ_IDLE;
    return XST_SUCCESS;
}

int pd_acq_start(u32 packet_limit)
{
    if (s_state != PD_ACQ_IDLE) return XST_FAILURE;
    if (configure_capture() != XST_SUCCESS) return XST_FAILURE;
    s_packet_limit = packet_limit;
    s_packets = 0U;
    s_dma_inflight = 0U;
    s_last_slot_sequence = ddr_read(PD_SLOT_SEQ);
    s_slot_ready_polls = 0U;
    s_last_error = NULL;
    s_state = PD_ACQ_RUNNING;
    return XST_SUCCESS;
}

void pd_acq_request_stop(void)
{
    if (s_state == PD_ACQ_RUNNING) s_state = PD_ACQ_STOPPING;
}

int pd_acq_poll(void)
{
    u32 dmasr, bytes;
    if (s_poll_hook != NULL) s_poll_hook();
    if (s_state == PD_ACQ_IDLE) return XST_SUCCESS;
    if (s_state == PD_ACQ_FAULT) return XST_FAILURE;
    if (s_state == PD_ACQ_STOPPING) {
        if (s_dma_inflight) {
            XAxiDma_Reset(&s_dma);
            while (!XAxiDma_ResetIsDone(&s_dma)) { }
            s_dma_inflight = 0U;
        }
        return stop_and_drain();
    }
    if (archive_new_snapshot() != XST_SUCCESS) return XST_FAILURE;
    if (!s_dma_inflight) return submit_dma();
    if (XAxiDma_Busy(&s_dma, XAXIDMA_DEVICE_TO_DMA)) {
        if (++s_dma_polls >= PD_DMA_POLL_LIMIT) {
            ++g_pd_acq.dma_errors;
            return fail("DMA completion timeout");
        }
        return XST_SUCCESS;
    }
    dmasr = dma_read(PD_S2MM_DMASR_OFFSET);
    bytes = dma_read(PD_S2MM_LENGTH_OFFSET);
    g_pd_acq.last_dma_status = dmasr;
    s_dma_inflight = 0U;
    if ((dmasr & (PD_DMASR_HALTED | PD_DMASR_ERROR_MASK)) || !(dmasr & PD_DMASR_IDLE)) {
        ++g_pd_acq.dma_errors;
        return fail("DMA completion failed");
    }
    Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, bytes);
    if (archive_event_packet(bytes) != XST_SUCCESS) return XST_FAILURE;
    ++s_packets;
    if (s_packet_limit != 0U && s_packets >= s_packet_limit) s_state = PD_ACQ_STOPPING;
    return XST_SUCCESS;
}

pd_acq_state_t pd_acq_state(void) { return s_state; }
u32 pd_acq_packets(void) { return s_packets; }
int pd_acq_set_default_packet_limit(u32 packet_limit)
{
    if (s_state != PD_ACQ_IDLE) return XST_FAILURE;
    s_default_packet_limit = packet_limit;
    return XST_SUCCESS;
}
u32 pd_acq_default_packet_limit(void) { return s_default_packet_limit; }
int pd_acq_get_event_by_sequence(u32 sequence, pd_event_record_t *record)
{
    u32 index;
    if (record == NULL || sequence >= g_pd_acq.event_sequence ||
        g_pd_acq.event_sequence - sequence > PD_EVENT_ARCHIVE_COUNT)
        return XST_FAILURE;
    index = sequence % PD_EVENT_ARCHIVE_COUNT;
    if (g_pd_acq.event[index].sequence != sequence) return XST_FAILURE;
    *record = g_pd_acq.event[index];
    return XST_SUCCESS;
}

int pd_acq_get_snapshot_by_sequence(u32 sequence, pd_snapshot_record_t *record)
{
    u32 index;
    if (record == NULL || sequence >= g_pd_acq.snapshot_sequence ||
        g_pd_acq.snapshot_sequence - sequence > PD_SNAP_ARCHIVE_COUNT)
        return XST_FAILURE;
    index = sequence % PD_SNAP_ARCHIVE_COUNT;
    if (g_pd_acq.snapshot[index].sequence != sequence) return XST_FAILURE;
    *record = g_pd_acq.snapshot[index];
    return XST_SUCCESS;
}

int pd_acq_recover(u32 *discarded_slots)
{
    u32 i, slot, status, valid;

    if (discarded_slots == NULL || s_state != PD_ACQ_FAULT) return XST_FAILURE;
    *discarded_slots = 0U;

    /* Stop new capture first.  Existing DataMover work is allowed to settle. */
    ddr_write(PD_SNAP_TRIG_CTRL, 0U);
    ddr_write(PD_DDR_CTRL, 0U);
    ddr_write(PD_SLOT_CTRL, 0U);       /* disable automatic slot allocation */
    ddr_write(PD_FREEZE_CTRL, 2U);     /* resume a held ring writer */

    for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i) {
        status = ddr_read(PD_SLOT_STATUS);
        if (!(ddr_read(PD_DDR_STATUS) & PD_DDR_COPY_BUSY) &&
            !(status & PD_SLOT_BUSY_MASK)) break;
    }
    if (i == PD_SHUTDOWN_POLL_LIMIT) return fail("recovery copy did not quiesce");

    /* A valid hardware slot has not reached the PS archive: discard explicitly. */
    valid = ddr_read(PD_SLOT_STATUS) & PD_SLOT_VALID_MASK;
    for (slot = 0U; slot < 4U; ++slot) {
        if (!(valid & (1U << slot))) continue;
        ddr_write(PD_SLOT_CTRL, 1U << (4U + slot));
        status = ddr_read(PD_SLOT_STATUS);
        if (!(status & (1U << (8U + slot)))) return fail("recovery slot lock failed");
        ddr_write(PD_SLOT_CTRL, 1U << (8U + slot));
        for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i)
            if (!(ddr_read(PD_SLOT_STATUS) & (1U << (8U + slot)))) break;
        if (i == PD_SHUTDOWN_POLL_LIMIT) return fail("recovery slot release failed");
        ++(*discarded_slots);
    }
    ddr_write(PD_SLOT_CTRL, 1U << 12); /* clear sticky slot status */
    if (reset_dma() != XST_SUCCESS) return XST_FAILURE;

    s_dma_inflight = 0U;
    s_dma_polls = 0U;
    s_slot_ready_polls = 0U;
    s_last_slot_sequence = ddr_read(PD_SLOT_SEQ);
    ++g_pd_acq.recovery_count;
    g_pd_acq.recovery_discarded_slots += *discarded_slots;
    g_pd_acq.last_ddr_status = ddr_read(PD_DDR_STATUS);
    g_pd_acq.last_slot_status = ddr_read(PD_SLOT_STATUS);
    g_pd_acq.last_dma_status = dma_read(PD_S2MM_DMASR_OFFSET);
    s_last_error = NULL;
    s_state = PD_ACQ_IDLE;
    return XST_SUCCESS;
}
const char *pd_acq_last_error(void) { return s_last_error == NULL ? "" : s_last_error; }
void pd_acq_set_poll_hook(pd_acq_poll_hook_t hook) { s_poll_hook = hook; }

void pd_acq_clear_metadata(void)
{
    if (s_state != PD_ACQ_IDLE) return;
    memset((void *)&g_pd_acq, 0, sizeof(g_pd_acq));
    g_pd_acq.magic = 0x50444151U;
    g_pd_acq.version = 3U;
}
