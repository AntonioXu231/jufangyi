/*
 * pd_acquisition_tcp_service_main.c -- TCP control front end for the
 * validated pd_acquisition_service capture engine.
 *
 * Transport contract, version 2:
 *   - TCP/IPv4, static 192.168.1.10, port 6001.
 *   - One line command per request, ASCII, CR/LF terminated, 95 bytes max.
 *   - Commands: HELP, STATUS, START [n], STOP, EVENT n, SNAP n, CLEAR,
 *     GET EVENT n offset bytes and GET SNAP n offset bytes.
 *   - Control responses are CR/LF-terminated ASCII.  GET replies with one
 *     ASCII DATA header followed by exactly the stated number of raw bytes.
 *   - Every GET is limited to 16 KiB and is sent from the main network-poll
 *     loop, never from the lwIP receive callback.
 *
 * UART remains enabled because pd_acquisition_service.c is included intact.
 * It remains the recovery/debug interface if Ethernet is disconnected.
 */

#include "xparameters.h"
#include "xil_printf.h"
#include "xil_types.h"
#include "xstatus.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "platform.h"
#include "platform_config.h"
#include "xil_cache.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pd_tcp_network_poll(void);
#define PD_ACQ_NO_MAIN
#define PD_ACQ_NETWORK_POLL pd_tcp_network_poll
/* Deliberately use an .inc file in the Vitis application directory.  Vitis
 * auto-adds every .c file to CMake; compiling this core separately would give
 * a second main()/g_acq definition. */
#include "pd_acquisition_service.inc"

#define PD_TCP_PORT          6001U
#define PD_TCP_LINE_BYTES    96U
#define PD_TCP_GET_MAX_BYTES 16384U
#define PD_TCP_SEND_BYTES    1024U
#define PD_TCP_MAC0          0x00U
#define PD_TCP_MAC1          0x0AU
#define PD_TCP_MAC2          0x35U
#define PD_TCP_MAC3          0x00U
#define PD_TCP_MAC4          0x01U
#define PD_TCP_MAC5          0x02U

/* platform.c from the Vitis lwIP echo template references this exact global
 * from its timer callback.  Retaining the name keeps the generated platform
 * support usable while the application owns the rest of the TCP service. */
struct netif echo_netif;
static struct tcp_pcb *g_client;
static char g_tcp_line[PD_TCP_LINE_BYTES];
static u32 g_tcp_len;

typedef struct {
    UINTPTR addr;
    u32 remaining;
    u32 offset;
    u32 bytes;
    u32 active;
} pd_tcp_transfer_t;

static pd_tcp_transfer_t g_transfer;

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
/* The Xilinx lwIP RAW-mode template exports these timer entry points but its
 * public lwIP headers do not declare them. */
void tcp_fasttmr(void);
void tcp_slowtmr(void);

static int tcp_reply(const char *text)
{
    err_t err;
    u16_t len;

    if (g_client == NULL) return -1;
    len = (u16_t)strlen(text);
    if (len == 0U) return 0;
    if (tcp_sndbuf(g_client) < len) {
        xil_printf("TCP reply dropped: send buffer full\r\n");
        return -1;
    }
    err = tcp_write(g_client, text, len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) return -1;
    (void)tcp_output(g_client);
    return 0;
}

static u32 tcp_crc32(const u8 *data, u32 bytes)
{
    u32 crc = 0xFFFFFFFFU;
    u32 i, bit;
    for (i = 0U; i < bytes; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return ~crc;
}

static int tcp_read_u32(char **cursor, u32 *value)
{
    char *end;
    unsigned long parsed;
    while (**cursor == ' ') ++(*cursor);
    if (!isdigit((unsigned char)**cursor)) return -1;
    parsed = strtoul(*cursor, &end, 0);
    if (end == *cursor || parsed > 0xFFFFFFFFUL) return -1;
    *value = (u32)parsed;
    *cursor = end;
    return 0;
}

static void tcp_transfer_pump(void)
{
    u16_t chunk;
    err_t err;

    if (!g_transfer.active || g_client == NULL) return;
    chunk = (g_transfer.remaining > PD_TCP_SEND_BYTES) ?
            PD_TCP_SEND_BYTES : (u16_t)g_transfer.remaining;
    if (tcp_sndbuf(g_client) < chunk) return;

    Xil_DCacheInvalidateRange(g_transfer.addr + g_transfer.offset, chunk);
    err = tcp_write(g_client, (const void *)(g_transfer.addr + g_transfer.offset),
                    chunk, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return;
    if (err != ERR_OK) {
        xil_printf("TCP GET aborted: tcp_write=%d\r\n", err);
        g_transfer.active = 0U;
        return;
    }
    g_transfer.offset += chunk;
    g_transfer.remaining -= chunk;
    if (g_transfer.remaining == 0U) g_transfer.active = 0U;
    (void)tcp_output(g_client);
}

static void tcp_reply_status(void)
{
    char out[192];
    (void)snprintf(out, sizeof(out),
        "STATUS run=%lu events=%lu snaps=%lu ev_ovw=%lu snap_ovw=%lu ddr=%08lx slot=%08lx dma=%08lx drops=%lu\r\n",
        (unsigned long)g_running, (unsigned long)g_acq.event_sequence,
        (unsigned long)g_acq.snapshot_sequence,
        (unsigned long)g_acq.event_overwrites,
        (unsigned long)g_acq.snapshot_overwrites,
        (unsigned long)ddr_read(DDR_STATUS), (unsigned long)ddr_read(SLOT_STATUS),
        (unsigned long)dma_read(S2MM_DMASR_OFFSET),
        (unsigned long)ddr_read(SLOT_DROPS));
    tcp_reply(out);
}

static void tcp_reply_event(u32 index)
{
    char out[160];
    if (index >= EVENT_ARCHIVE_COUNT ||
        g_acq.event[index].sequence >= g_acq.event_sequence) {
        tcp_reply("ERR EVENT index has no valid record\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "EVENT index=%lu seq=%lu addr=%08lx bytes=%lu peaks=%lu cycles=%lu\r\n",
        (unsigned long)index, (unsigned long)g_acq.event[index].sequence,
        (unsigned long)g_acq.event[index].ddr_addr,
        (unsigned long)g_acq.event[index].bytes,
        (unsigned long)g_acq.event[index].peak_words,
        (unsigned long)g_acq.event[index].cycle_words);
    tcp_reply(out);
}

static void tcp_reply_snapshot(u32 index)
{
    char out[192];
    if (index >= SNAP_ARCHIVE_COUNT ||
        g_acq.snapshot[index].sequence >= g_acq.snapshot_sequence) {
        tcp_reply("ERR SNAP index has no valid record\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "SNAP index=%lu seq=%lu hw_slot=%lu src=%08lx dst=%08lx bytes=%lu flags=%08lx\r\n",
        (unsigned long)index, (unsigned long)g_acq.snapshot[index].sequence,
        (unsigned long)g_acq.snapshot[index].source_slot,
        (unsigned long)g_acq.snapshot[index].source_addr,
        (unsigned long)g_acq.snapshot[index].archive_addr,
        (unsigned long)g_acq.snapshot[index].bytes,
        (unsigned long)g_acq.snapshot[index].flags);
    tcp_reply(out);
}

static void tcp_start_get(char *line)
{
    char *p = line + 4;
    const char *kind;
    UINTPTR base;
    u32 index, offset, bytes, available, crc;
    char out[192];

    if (g_running) {
        tcp_reply("ERR GET is accepted only while idle\r\n");
        return;
    }
    while (*p == ' ') ++p;
    if (strncmp(p, "EVENT", 5) == 0 && p[5] == ' ') {
        kind = "EVENT";
        p += 5;
        if (tcp_read_u32(&p, &index) != 0 || index >= EVENT_ARCHIVE_COUNT ||
            g_acq.event[index].sequence >= g_acq.event_sequence) {
            tcp_reply("ERR GET EVENT has no valid record\r\n");
            return;
        }
        base = (UINTPTR)g_acq.event[index].ddr_addr;
        available = g_acq.event[index].bytes;
    } else if (strncmp(p, "SNAP", 4) == 0 && p[4] == ' ') {
        kind = "SNAP";
        p += 4;
        if (tcp_read_u32(&p, &index) != 0 || index >= SNAP_ARCHIVE_COUNT ||
            g_acq.snapshot[index].sequence >= g_acq.snapshot_sequence) {
            tcp_reply("ERR GET SNAP has no valid record\r\n");
            return;
        }
        base = (UINTPTR)g_acq.snapshot[index].archive_addr;
        available = g_acq.snapshot[index].bytes;
    } else {
        tcp_reply("ERR usage: GET EVENT|SNAP index offset bytes\r\n");
        return;
    }

    if (tcp_read_u32(&p, &offset) != 0 || tcp_read_u32(&p, &bytes) != 0) {
        tcp_reply("ERR usage: GET EVENT|SNAP index offset bytes\r\n");
        return;
    }
    while (*p == ' ') ++p;
    if (*p != 0 || offset >= available || bytes == 0U ||
        bytes > PD_TCP_GET_MAX_BYTES || bytes > available - offset) {
        tcp_reply("ERR GET range; bytes must be 1..16384 inside record\r\n");
        return;
    }

    Xil_DCacheInvalidateRange(base + offset, bytes);
    crc = tcp_crc32((const u8 *)(base + offset), bytes);
    (void)snprintf(out, sizeof(out),
        "DATA V2 kind=%s index=%lu offset=%lu bytes=%lu crc32=%08lx\r\n",
        kind, (unsigned long)index, (unsigned long)offset,
        (unsigned long)bytes, (unsigned long)crc);
    if (tcp_reply(out) != 0) return;

    g_transfer.addr = base;
    g_transfer.offset = offset;
    g_transfer.remaining = bytes;
    g_transfer.bytes = bytes;
    g_transfer.active = 1U;
}

static void tcp_execute_command(char *line)
{
    if (g_transfer.active) {
        /* A control reply here would be interleaved with raw bytes.  Clients
         * must read the declared payload before issuing their next command. */
        return;
    }
    if (strcmp(line, "HELP") == 0) {
        tcp_reply("CMD: START [n] | STOP | STATUS | EVENT n | SNAP n | GET EVENT|SNAP n offset bytes | CLEAR\r\n");
    } else if (strncmp(line, "START", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        if (g_running) {
            tcp_reply("ERR already running\r\n");
        } else {
            g_start_limit = parse_u32(line + 5, PD_ACQ_PACKET_LIMIT);
            g_start_request = 1U;
            tcp_reply("OK start accepted\r\n");
        }
    } else if (strcmp(line, "STOP") == 0) {
        if (!g_running) tcp_reply("OK already idle\r\n");
        else {
            g_stop_request = 1U;
            tcp_reply("OK stop requested\r\n");
        }
    } else if (strcmp(line, "STATUS") == 0) {
        tcp_reply_status();
    } else if (strncmp(line, "EVENT ", 6) == 0) {
        tcp_reply_event(parse_u32(line + 6, EVENT_ARCHIVE_COUNT));
    } else if (strncmp(line, "SNAP ", 5) == 0) {
        tcp_reply_snapshot(parse_u32(line + 5, SNAP_ARCHIVE_COUNT));
    } else if (strncmp(line, "GET ", 4) == 0) {
        tcp_start_get(line);
    } else if (strcmp(line, "CLEAR") == 0) {
        if (g_running) {
            tcp_reply("ERR CLEAR is accepted only while idle\r\n");
        } else {
            clear_metadata();
            tcp_reply("OK CLEAR metadata\r\n");
        }
    } else if (line[0] != 0) {
        tcp_reply("ERR unknown command; type HELP\r\n");
    }
}

static err_t pd_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    u16_t i;
    char c;
    (void)arg;
    (void)err;

    if (p == NULL) {
        tcp_recv(pcb, NULL);
        if (g_client == pcb) g_client = NULL;
        g_transfer.active = 0U;
        (void)tcp_close(pcb);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    for (i = 0U; i < p->tot_len; ++i) {
        (void)pbuf_copy_partial(p, &c, 1U, i);
        if (c == '\r' || c == '\n') {
            if (g_tcp_len != 0U) {
                g_tcp_line[g_tcp_len] = 0;
                tcp_execute_command(g_tcp_line);
                g_tcp_len = 0U;
            }
        } else if (c >= ' ' && c <= '~') {
            if (g_tcp_len + 1U < PD_TCP_LINE_BYTES) {
                g_tcp_line[g_tcp_len++] = c;
            } else {
                g_tcp_len = 0U;
                tcp_reply("ERR command too long\r\n");
            }
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void pd_tcp_err_cb(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    g_client = NULL;
    g_tcp_len = 0U;
    g_transfer.active = 0U;
}

static err_t pd_tcp_accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || g_client != NULL) {
        (void)tcp_close(pcb);
        return ERR_ABRT;
    }
    g_client = pcb;
    g_tcp_len = 0U;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, pd_tcp_recv_cb);
    tcp_err(pcb, pd_tcp_err_cb);
    tcp_reply("PD_ACQ TCP V2 READY; type HELP\r\n");
    return ERR_OK;
}

static int tcp_server_start(void)
{
    struct tcp_pcb *listener;
    err_t err;

    listener = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (listener == NULL) return -1;
    err = tcp_bind(listener, IP_ANY_TYPE, PD_TCP_PORT);
    if (err != ERR_OK) return -2;
    listener = tcp_listen(listener);
    if (listener == NULL) return -3;
    tcp_accept(listener, pd_tcp_accept_cb);
    return 0;
}

static void pd_tcp_network_poll(void)
{
    if (TcpFastTmrFlag) {
        tcp_fasttmr();
        TcpFastTmrFlag = 0;
    }
    if (TcpSlowTmrFlag) {
        tcp_slowtmr();
        TcpSlowTmrFlag = 0;
    }
    xemacif_input(&echo_netif);
    tcp_transfer_pump();
}

int main(void)
{
    XAxiDma_Config *cfg;
    ip_addr_t ipaddr, netmask, gw;
    unsigned char mac[6] = {
        PD_TCP_MAC0, PD_TCP_MAC1, PD_TCP_MAC2,
        PD_TCP_MAC3, PD_TCP_MAC4, PD_TCP_MAC5
    };
    u32 packets = 0U;
    u32 last_slot_sequence = 0U;

    memset((void *)&g_acq, 0, sizeof(g_acq));
    g_acq.magic = 0x50444151U;
    g_acq.version = 1U;

    init_platform();
    IP4_ADDR(&ipaddr, 192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 1, 1);
    lwip_init();
    if (xemac_add(&echo_netif, &ipaddr, &netmask, &gw, mac,
                  PLATFORM_EMAC_BASEADDR) == NULL) {
        xil_printf("TCP_ACQ_FAIL: xemac_add failed\r\n");
        for (;;) __asm__ volatile ("wfi");
    }
    netif_set_default(&echo_netif);
#ifndef SDT
    platform_enable_interrupts();
#endif
    netif_set_up(&echo_netif);

    xil_printf("\r\n--- pd acquisition TCP service V2 ---\r\n");
    xil_printf("IP=192.168.1.10 TCP=%u; UART remains enabled\r\n", PD_TCP_PORT);
    cfg = XAxiDma_LookupConfig(DMA_LOOKUP_ARG);
    if (cfg == NULL) halt_failure("DMA config not found");
    if (XAxiDma_CfgInitialize(&g_dma, cfg) != XST_SUCCESS) halt_failure("DMA init failed");
    if (XAxiDma_HasSg(&g_dma)) halt_failure("expected Simple-mode DMA");
    XAxiDma_IntrDisable(&g_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_Reset(&g_dma);
    while (!XAxiDma_ResetIsDone(&g_dma)) { }
    if (tcp_server_start() != 0) halt_failure("TCP listen failed");

    xil_printf("TCP_ACQ_READY: connect 192.168.1.10:%u and send HELP\r\n", PD_TCP_PORT);
    for (;;) {
        uart_poll();
        pd_tcp_network_poll();

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
        if ((packets % REPORT_PERIOD) == 0U) {
            xil_printf("ACQ packets=%u events=%u snaps=%u ev_ovw=%u snap_ovw=%u drops=%u\r\n",
                       packets, g_acq.event_sequence, g_acq.snapshot_sequence,
                       g_acq.event_overwrites, g_acq.snapshot_overwrites,
                       ddr_read(SLOT_DROPS));
        }
        if (g_start_limit != 0U && packets >= g_start_limit) {
            archive_new_snapshot(&last_slot_sequence);
            clean_stop(&last_slot_sequence);
            g_running = 0U;
            xil_printf("ACQ_SERVICE_PASS packets=%u events=%u snapshots=%u ev_ovw=%u snap_ovw=%u\r\n",
                       packets, g_acq.event_sequence, g_acq.snapshot_sequence,
                       g_acq.event_overwrites, g_acq.snapshot_overwrites);
        }
    }
}
