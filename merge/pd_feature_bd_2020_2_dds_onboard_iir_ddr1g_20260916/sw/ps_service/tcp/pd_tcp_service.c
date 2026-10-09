/* TCP API 18 front end; acquisition remains owned by pd_acquisition_core. */
#include "pd_tcp_service.h"
#include "pd_acquisition.h"
#include "pd_hw_map.h"
#include "pd_snapshot_unpack.h"
#include "pd_spectrum.h"
#include "pd_analysis.h"
#include "pd_event_decode.h"
#include "pd_prpd.h"

#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "netif/xadapter.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD_TCP_PORT          6001U
#define PD_TCP_LINE_BYTES    96U
#define PD_TCP_GET_MAX_BYTES 16384U
#define PD_TCP_SEND_BYTES    4096U
/* Bound each main-loop pass while amortizing TCP output overhead. */
#define PD_TCP_SEND_BURST_BYTES (PD_TCP_SEND_BYTES * 4U)
#define PD_SCOPE_SAMPLE_BYTES    6U
#define PD_SCOPE_DEFAULT_SAMPLES 1024U
#define PD_SCOPE_MAX_SAMPLES     2048U
#define PD_SCOPE_ENV_BINS        1024U
#define PD_SCOPE_ENV_CHANNELS    4U
#define PD_SCOPE_ENV_BYTES       (PD_SCOPE_ENV_BINS * PD_SCOPE_ENV_CHANNELS * 4U)
#define PD_SCOPE_CYCLE_SAMPLES   (PD_SPECTRUM_DEFAULT_FS_HZ / 50U)
#define PD_SCOPE_ENV_STEP_SAMPLES 32768U
#define PD_SCOPE_PEAK_BATCH_MAX_PACKETS 32U
#define PD_SCOPE_PEAK_BATCH_BYTES       131072U
/* 24 KiB is 1024 complete four-sample (24-byte) PL write blocks. */
#define PD_SCOPE_SAFETY_BYTES    24576U

extern struct netif echo_netif;
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
void tcp_fasttmr(void);
void tcp_slowtmr(void);

static struct tcp_pcb *s_client;
static char s_line[PD_TCP_LINE_BYTES];
static u32 s_line_len;
static u32 s_fft_sample_rate_hz = PD_SPECTRUM_DEFAULT_FS_HZ;
static u32 s_alert_channel_mask = 0xFU;
static s32 s_alert_delta_permille; /* Zero intentionally disables rule verdicts. */
static const u32 s_crc32_nibble_table[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
};

typedef struct {
    UINTPTR addr;
    u32 remaining;
    u32 offset;
    u32 active;
    u32 invalidate_cache;
} pd_tcp_transfer_t;
static pd_tcp_transfer_t s_transfer;

typedef struct {
    u32 enabled;
    u32 samples;
    u32 sequence;
    u32 peak_cursor;
    u32 last_frame_sequence;
    u32 last_frame_valid;
    u32 peak_cursor_valid;
} pd_scope_state_t;

static pd_scope_state_t s_scope = {0U, PD_SCOPE_DEFAULT_SAMPLES, 0U, 0U, 0U, 0U, 0U};
static u8 s_scope_buffer[PD_SCOPE_MAX_SAMPLES * PD_SCOPE_SAMPLE_BYTES];
static u16 s_scope_fft_bins[PD_FFT_POINTS / 2U + 1U];
static u8 s_scope_fft_buffer[(PD_FFT_POINTS / 2U + 1U) * 2U];
static u8 s_snapshot_fft_wave_buffer[(PD_FFT_POINTS * 2U) +
                                     ((PD_FFT_POINTS / 2U + 1U) * 2U)];
typedef struct {
    u32 active;
    u32 ready;
    u32 cached_valid;
    u32 snapshot_sequence;
    u32 snapshot_bytes;
    u32 snapshot_addr;
    u32 sample_count;
    u32 sample_cursor;
    u32 bin_cursor;
    u32 lock_mask;
    u32 checksum;
    s16 minimum[PD_SCOPE_ENV_BINS][PD_SCOPE_ENV_CHANNELS];
    s16 maximum[PD_SCOPE_ENV_BINS][PD_SCOPE_ENV_CHANNELS];
    u8 payload[PD_SCOPE_ENV_BYTES];
    char header[192];
} pd_scope_envelope_job_t;
static pd_scope_envelope_job_t s_scope_envelope;
/* Stage one complete event packet before TCP transfer, so the archive ring can
 * continue receiving packets while this connection drains its own copy. */
static u64 s_scope_peak_buffer[PD_EVENT_ARCHIVE_STRIDE / sizeof(u64)];
/* Batch payload records are {sequence:u32, bytes:u32, raw event bytes}.
 * This amortizes TCP command/response latency without thinning event words. */
static u8 s_scope_peak_batch_buffer[PD_SCOPE_PEAK_BATCH_BYTES];
static u32 s_scope_peak_skipped;

static u32 ddr_read(u32 off) { return Xil_In32(PD_DDR_BASE + off); }
static u32 dma_read(u32 off) { return Xil_In32(PD_DMA_BASE + off); }

static int reply(const char *text)
{
    u16_t len;
    err_t err;
    if (s_client == NULL) return -1;
    len = (u16_t)strlen(text);
    if (len == 0U) return 0;
    if (tcp_sndbuf(s_client) < len) return -1;
    err = tcp_write(s_client, text, len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) return -1;
    (void)tcp_output(s_client);
    return 0;
}

static u32 crc32(const u8 *data, u32 bytes)
{
    u32 crc = 0xFFFFFFFFU, i;
    for (i = 0U; i < bytes; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ s_crc32_nibble_table[crc & 0x0FU];
        crc = (crc >> 4) ^ s_crc32_nibble_table[crc & 0x0FU];
    }
    return ~crc;
}

static void store_u32_le(u8 *destination, u32 value)
{
    destination[0] = (u8)(value & 0xFFU);
    destination[1] = (u8)((value >> 8) & 0xFFU);
    destination[2] = (u8)((value >> 16) & 0xFFU);
    destination[3] = (u8)((value >> 24) & 0xFFU);
}

static int read_u32(char **cursor, u32 *value)
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

static u32 parse_u32(const char *text, u32 fallback)
{
    char *cursor = (char *)text;
    u32 value;
    return read_u32(&cursor, &value) == 0 ? value : fallback;
}

static int parse_single_u32(const char *text, u32 *value)
{
    char *cursor = (char *)text;
    if (read_u32(&cursor, value) != 0) return -1;
    while (*cursor == ' ') ++cursor;
    return *cursor == 0 ? 0 : -1;
}

static int parse_one_or_two_u32(char *text, u32 *first, u32 *second)
{
    char *cursor = text;
    if (read_u32(&cursor, first) != 0) return -1;
    while (*cursor == ' ') ++cursor;
    if (*cursor == 0) { *second = 0U; return 0; }
    if (read_u32(&cursor, second) != 0) return -1;
    while (*cursor == ' ') ++cursor;
    return *cursor == 0 ? 0 : -1;
}

/* SCOPE EVENT accepts: record_seq [samples [channel evt_seq]]. */
static int parse_scope_event_args(char *text, u32 *record_sequence, u32 *samples,
                                 u32 *channel, u32 *event_word_sequence,
                                 u32 *has_event_selector)
{
    char *cursor = text;
    *samples = 0U;
    *channel = 0U;
    *event_word_sequence = 0U;
    *has_event_selector = 0U;
    if (read_u32(&cursor, record_sequence) != 0) return -1;
    while (*cursor == ' ') ++cursor;
    if (*cursor == 0) return 0;
    if (read_u32(&cursor, samples) != 0) return -1;
    while (*cursor == ' ') ++cursor;
    if (*cursor == 0) return 0;
    if (read_u32(&cursor, channel) != 0 || *channel >= PD_SNAPSHOT_CHANNELS)
        return -1;
    if (read_u32(&cursor, event_word_sequence) != 0 ||
        *event_word_sequence > 0x01FFFFFFU)
        return -1;
    while (*cursor == ' ') ++cursor;
    if (*cursor != 0) return -1;
    *has_event_selector = 1U;
    return 0;
}

static void transfer_pump(void)
{
    u16_t chunk, available;
    u32 chunk_limit, queued = 0U;
    err_t err;
    if (!s_transfer.active || s_client == NULL) return;
    while (s_transfer.active && queued < PD_TCP_SEND_BURST_BYTES) {
        available = tcp_sndbuf(s_client);
        if (available == 0U) break;
        chunk_limit = PD_TCP_SEND_BYTES;
        if (chunk_limit > available) chunk_limit = available;
        if (chunk_limit > PD_TCP_SEND_BURST_BYTES - queued)
            chunk_limit = PD_TCP_SEND_BURST_BYTES - queued;
        chunk = s_transfer.remaining > chunk_limit ? (u16_t)chunk_limit :
                (u16_t)s_transfer.remaining;
        if (s_transfer.invalidate_cache)
            Xil_DCacheInvalidateRange(s_transfer.addr + s_transfer.offset, chunk);
        err = tcp_write(s_client, (const void *)(s_transfer.addr + s_transfer.offset),
                        chunk, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) break;
        if (err != ERR_OK) {
            xil_printf("TCP GET aborted: tcp_write=%d\r\n", err);
            s_transfer.active = 0U;
            break;
        }
        s_transfer.offset += chunk;
        s_transfer.remaining -= chunk;
        queued += chunk;
        if (s_transfer.remaining == 0U) s_transfer.active = 0U;
    }
    if (queued != 0U) (void)tcp_output(s_client);
}

#include "modules/metadata/pd_tcp_metadata.inc"
#include "modules/spectrum/pd_tcp_spectrum.inc"
#include "modules/analysis/pd_tcp_analysis.inc"
#include "modules/scope/pd_tcp_scope_capture.inc"
#include "modules/scope/pd_tcp_scope_events.inc"
#include "modules/scope/pd_tcp_scope_envelope.inc"
#include "modules/commands/pd_tcp_commands.inc"
