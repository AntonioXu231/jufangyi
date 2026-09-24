/* UART smoke front end for the modular PS-1 acquisition core. */
#include "pd_acquisition.h"
#include "pd_hw_map.h"

#include "xil_io.h"
#include "xil_printf.h"
#include "xuartps_hw.h"
#include <string.h>

#ifndef XPAR_XUARTPS_0_BASEADDR
# error "Set stdin/stdout to ps7_uart_1 (or update this UART selection in the BSP)."
#endif

#define PD_UART_BASE       XPAR_XUARTPS_0_BASEADDR
#define PD_UART_LINE_BYTES 48U

static char s_line[PD_UART_LINE_BYTES];
static u32 s_line_len;

static u32 ddr_read(u32 off) { return Xil_In32(PD_DDR_BASE + off); }
static u32 dma_read(u32 off) { return Xil_In32(PD_DMA_BASE + off); }

static u32 parse_u32(const char *p, u32 default_value)
{
    u32 value = 0U, any = 0U;
    while (*p == ' ') ++p;
    while (*p >= '0' && *p <= '9') {
        any = 1U;
        value = value * 10U + (u32)(*p - '0');
        ++p;
    }
    return any ? value : default_value;
}

static void print_status(void)
{
    xil_printf("STATUS state=%u packets=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u ",
               (u32)pd_acq_state(), pd_acq_packets(), g_pd_acq.event_sequence,
               g_pd_acq.snapshot_sequence, g_pd_acq.event_overwrites,
               g_pd_acq.snapshot_overwrites);
    xil_printf("ddr=%08x slot=%08x dma=%08x drops=%u err=%s\r\n",
               ddr_read(PD_DDR_STATUS), ddr_read(PD_SLOT_STATUS),
               dma_read(PD_S2MM_DMASR_OFFSET), ddr_read(PD_SLOT_DROPS),
               pd_acq_last_error());
}

static void print_event(u32 index)
{
    pd_event_record_t r;
    if (index >= PD_EVENT_ARCHIVE_COUNT ||
        g_pd_acq.event[index].sequence >= g_pd_acq.event_sequence) {
        xil_printf("ERR EVENT index has no valid record\r\n");
        return;
    }
    r = g_pd_acq.event[index];
    xil_printf("EVENT index=%u seq=%u addr=%08x bytes=%u peaks=%u cycles=%u\r\n",
               index, r.sequence, r.ddr_addr, r.bytes, r.peak_words, r.cycle_words);
}

static void print_snapshot(u32 index)
{
    pd_snapshot_record_t r;
    if (index >= PD_SNAP_ARCHIVE_COUNT ||
        g_pd_acq.snapshot[index].sequence >= g_pd_acq.snapshot_sequence) {
        xil_printf("ERR SNAP index has no valid record\r\n");
        return;
    }
    r = g_pd_acq.snapshot[index];
    xil_printf("SNAP index=%u seq=%u hw_slot=%u src=%08x dst=%08x bytes=%u flags=%08x\r\n",
               index, r.sequence, r.source_slot, r.source_addr, r.archive_addr,
               r.bytes, r.flags);
}

static void execute_command(char *line)
{
    if (strcmp(line, "HELP") == 0) {
        xil_printf("CMD: START [packets] | STOP | STATUS | EVENT n | SNAP n | CLEAR\r\n");
    } else if (strncmp(line, "START", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        if (pd_acq_start(parse_u32(line + 5, 128U)) == XST_SUCCESS)
            xil_printf("OK START accepted\r\n");
        else xil_printf("ERR START state=%u err=%s\r\n", (u32)pd_acq_state(), pd_acq_last_error());
    } else if (strcmp(line, "STOP") == 0) {
        pd_acq_request_stop();
        xil_printf("OK STOP requested\r\n");
    } else if (strcmp(line, "STATUS") == 0) {
        print_status();
    } else if (strncmp(line, "EVENT ", 6) == 0) {
        print_event(parse_u32(line + 6, PD_EVENT_ARCHIVE_COUNT));
    } else if (strncmp(line, "SNAP ", 5) == 0) {
        print_snapshot(parse_u32(line + 5, PD_SNAP_ARCHIVE_COUNT));
    } else if (strcmp(line, "CLEAR") == 0) {
        if (pd_acq_state() == PD_ACQ_IDLE) {
            pd_acq_clear_metadata();
            xil_printf("OK metadata cleared\r\n");
        } else xil_printf("ERR CLEAR requires idle\r\n");
    } else if (line[0] != 0) {
        xil_printf("ERR unknown command; type HELP\r\n");
    }
}

static void uart_poll(void)
{
    while (XUartPs_IsReceiveData(PD_UART_BASE)) {
        char c = (char)XUartPs_RecvByte(PD_UART_BASE);
        if (c == '\r' || c == '\n') {
            if (s_line_len != 0U) {
                s_line[s_line_len] = 0;
                execute_command(s_line);
                s_line_len = 0U;
            }
        } else if (c == 8 || c == 127) {
            if (s_line_len != 0U) --s_line_len;
        } else if (c >= ' ' && c <= '~') {
            if (s_line_len + 1U < PD_UART_LINE_BYTES) s_line[s_line_len++] = c;
            else { s_line_len = 0U; xil_printf("ERR command too long\r\n"); }
        }
    }
}

int main(void)
{
    if (pd_acq_init() != XST_SUCCESS) {
        xil_printf("PD_ACQ_INIT_FAIL: %s\r\n", pd_acq_last_error());
        for (;;) __asm__ volatile ("wfi");
    }
    xil_printf("\r\n--- pd modular acquisition service PS-1 ---\r\n");
    xil_printf("READY: type HELP, then START 128 (or START 0 for continuous)\r\n");
    for (;;) {
        uart_poll();
        (void)pd_acq_poll();
    }
}
