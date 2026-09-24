/* TCP V2 front end.  It owns transport only; acquisition is owned by core. */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD_TCP_PORT          6001U
#define PD_TCP_LINE_BYTES    96U
#define PD_TCP_GET_MAX_BYTES 16384U
#define PD_TCP_SEND_BYTES    1024U
#define PD_SCOPE_SAMPLE_BYTES    6U
#define PD_SCOPE_DEFAULT_SAMPLES 1024U
#define PD_SCOPE_MAX_SAMPLES     2048U
#define PD_SCOPE_MAX_PEAKS       256U
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
} pd_scope_state_t;

static pd_scope_state_t s_scope = {0U, PD_SCOPE_DEFAULT_SAMPLES, 0U, 0U};
static u8 s_scope_buffer[PD_SCOPE_MAX_SAMPLES * PD_SCOPE_SAMPLE_BYTES];
static u64 s_scope_peak_buffer[PD_SCOPE_MAX_PEAKS];

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
    u32 crc = 0xFFFFFFFFU, i, bit;
    for (i = 0U; i < bytes; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return ~crc;
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

static void transfer_pump(void)
{
    u16_t chunk;
    err_t err;
    if (!s_transfer.active || s_client == NULL) return;
    chunk = s_transfer.remaining > PD_TCP_SEND_BYTES ? PD_TCP_SEND_BYTES :
            (u16_t)s_transfer.remaining;
    if (tcp_sndbuf(s_client) < chunk) return;
    if (s_transfer.invalidate_cache)
        Xil_DCacheInvalidateRange(s_transfer.addr + s_transfer.offset, chunk);
    err = tcp_write(s_client, (const void *)(s_transfer.addr + s_transfer.offset),
                    chunk, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return;
    if (err != ERR_OK) {
        xil_printf("TCP GET aborted: tcp_write=%d\r\n", err);
        s_transfer.active = 0U;
        return;
    }
    s_transfer.offset += chunk;
    s_transfer.remaining -= chunk;
    if (s_transfer.remaining == 0U) s_transfer.active = 0U;
    (void)tcp_output(s_client);
}

static void reply_status(void)
{
    char out[256];
    (void)snprintf(out, sizeof(out),
        "STATUS state=%lu packets=%lu events=%lu snaps=%lu analysis=%lu sweeps=%lu ev_ovw=%lu snap_ovw=%lu recov=%lu disc=%lu ddr=%08lx slot=%08lx dma=%08lx drops=%lu err=%s\r\n",
        (unsigned long)pd_acq_state(), (unsigned long)pd_acq_packets(),
        (unsigned long)g_pd_acq.event_sequence, (unsigned long)g_pd_acq.snapshot_sequence,
        (unsigned long)pd_analysis_next_sequence(),
        (unsigned long)pd_analysis_sweep_next_sequence(),
        (unsigned long)g_pd_acq.event_overwrites, (unsigned long)g_pd_acq.snapshot_overwrites,
        (unsigned long)g_pd_acq.recovery_count,
        (unsigned long)g_pd_acq.recovery_discarded_slots,
        (unsigned long)ddr_read(PD_DDR_STATUS), (unsigned long)ddr_read(PD_SLOT_STATUS),
        (unsigned long)dma_read(PD_S2MM_DMASR_OFFSET),
        (unsigned long)ddr_read(PD_SLOT_DROPS), pd_acq_last_error());
    (void)reply(out);
}

static void reply_config(void)
{
    char out[320];
    (void)snprintf(out, sizeof(out),
        "CONFIG api=13 default_limit=%lu event_slots=%u event_stride=%lu snap_slots=%u snap_stride=%lu analysis_slots=%u sweep_slots=%u sweep_windows=%u prpd_bins=%u scope=%lu/%lu alert_mask=%lx alert_delta=%ld get_max=%u state=%lu\r\n",
        (unsigned long)pd_acq_default_packet_limit(), PD_EVENT_ARCHIVE_COUNT,
        (unsigned long)PD_EVENT_ARCHIVE_STRIDE, PD_SNAP_ARCHIVE_COUNT,
        (unsigned long)PD_SNAP_ARCHIVE_STRIDE, PD_ANALYSIS_ARCHIVE_COUNT,
        PD_ANALYSIS_ARCHIVE_COUNT, PD_ANALYSIS_SWEEP_WINDOWS, PD_PRPD_PHASE_BINS,
        (unsigned long)s_scope.enabled, (unsigned long)s_scope.samples,
        (unsigned long)s_alert_channel_mask, (long)s_alert_delta_permille,
        PD_TCP_GET_MAX_BYTES,
        (unsigned long)pd_acq_state());
    (void)reply(out);
}

static void reply_report(void)
{
    u32 event_next = g_pd_acq.event_sequence;
    u32 snapshot_next = g_pd_acq.snapshot_sequence;
    u32 analysis_next = pd_analysis_next_sequence();
    u32 sweep_next = pd_analysis_sweep_next_sequence();
    u32 event_first = event_next > PD_EVENT_ARCHIVE_COUNT ?
                      event_next - PD_EVENT_ARCHIVE_COUNT : 0U;
    u32 snapshot_first = snapshot_next > PD_SNAP_ARCHIVE_COUNT ?
                         snapshot_next - PD_SNAP_ARCHIVE_COUNT : 0U;
    u32 analysis_first = analysis_next > PD_ANALYSIS_ARCHIVE_COUNT ?
                         analysis_next - PD_ANALYSIS_ARCHIVE_COUNT : 0U;
    u32 sweep_first = sweep_next > PD_ANALYSIS_ARCHIVE_COUNT ?
                      sweep_next - PD_ANALYSIS_ARCHIVE_COUNT : 0U;
    char out[320];

    (void)snprintf(out, sizeof(out),
        "REPORT api=13 state=%lu event_seq=[%lu,%lu) snap_seq=[%lu,%lu) analysis_seq=[%lu,%lu) sweep_seq=[%lu,%lu) alert_mask=0x%lx alert_delta=%ld ddr=%08lx slot=%08lx dma=%08lx drops=%lu err=%s\r\n",
        (unsigned long)pd_acq_state(), (unsigned long)event_first,
        (unsigned long)event_next, (unsigned long)snapshot_first,
        (unsigned long)snapshot_next, (unsigned long)analysis_first,
        (unsigned long)analysis_next, (unsigned long)sweep_first,
        (unsigned long)sweep_next, (unsigned long)s_alert_channel_mask,
        (long)s_alert_delta_permille, (unsigned long)ddr_read(PD_DDR_STATUS),
        (unsigned long)ddr_read(PD_SLOT_STATUS),
        (unsigned long)dma_read(PD_S2MM_DMASR_OFFSET),
        (unsigned long)ddr_read(PD_SLOT_DROPS), pd_acq_last_error());
    (void)reply(out);
}

static void reply_event(u32 index)
{
    char out[160];
    pd_event_record_t r;
    if (index >= PD_EVENT_ARCHIVE_COUNT ||
        g_pd_acq.event[index].sequence >= g_pd_acq.event_sequence) {
        (void)reply("ERR EVENT index has no valid record\r\n");
        return;
    }
    r = g_pd_acq.event[index];
    (void)snprintf(out, sizeof(out),
        "EVENT index=%lu seq=%lu addr=%08lx bytes=%lu peaks=%lu cycles=%lu\r\n",
        (unsigned long)index, (unsigned long)r.sequence, (unsigned long)r.ddr_addr,
        (unsigned long)r.bytes, (unsigned long)r.peak_words, (unsigned long)r.cycle_words);
    (void)reply(out);
}

static void reply_event_detail(const pd_event_record_t *record, u32 index)
{
    pd_event_summary_t summary;
    char out[480];

    if (pd_event_summarize(record, &summary) != XST_SUCCESS) {
        (void)reply("ERR EVENT DETAIL decode failed; packet contract mismatch\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "EVENT_DETAIL index=%lu seq=%lu bytes=%lu peaks=%lu cycles=%lu ch0=p%lu/q%lu/c%lu/n%lu/qmax%lu/cyc%lu ch1=p%lu/q%lu/c%lu/n%lu/qmax%lu/cyc%lu ch2=p%lu/q%lu/c%lu/n%lu/qmax%lu/cyc%lu ch3=p%lu/q%lu/c%lu/n%lu/qmax%lu/cyc%lu\r\n",
        (unsigned long)index, (unsigned long)summary.event_sequence,
        (unsigned long)summary.bytes, (unsigned long)summary.peak_words,
        (unsigned long)summary.cycle_words,
        (unsigned long)summary.peak_count[0], (unsigned long)summary.q_abs_max[0],
        (unsigned long)summary.cycle_count[0], (unsigned long)summary.cycle_event_total[0],
        (unsigned long)summary.cycle_qmax[0], (unsigned long)summary.cycle_last_index[0],
        (unsigned long)summary.peak_count[1], (unsigned long)summary.q_abs_max[1],
        (unsigned long)summary.cycle_count[1], (unsigned long)summary.cycle_event_total[1],
        (unsigned long)summary.cycle_qmax[1], (unsigned long)summary.cycle_last_index[1],
        (unsigned long)summary.peak_count[2], (unsigned long)summary.q_abs_max[2],
        (unsigned long)summary.cycle_count[2], (unsigned long)summary.cycle_event_total[2],
        (unsigned long)summary.cycle_qmax[2], (unsigned long)summary.cycle_last_index[2],
        (unsigned long)summary.peak_count[3], (unsigned long)summary.q_abs_max[3],
        (unsigned long)summary.cycle_count[3], (unsigned long)summary.cycle_event_total[3],
        (unsigned long)summary.cycle_qmax[3], (unsigned long)summary.cycle_last_index[3]);
    (void)reply(out);
}

static void start_event_detail(char *line)
{
    char *p = line + 13; /* "EVENT DETAIL " */
    pd_event_record_t record;
    u32 index, sequence;

    if (strncmp(p, "SEQ ", 4) == 0) {
        if (parse_single_u32(p + 4, &sequence) != 0 ||
            pd_acq_get_event_by_sequence(sequence, &record) != XST_SUCCESS) {
            (void)reply("ERR EVENT DETAIL sequence is outside the retained archive window\r\n");
            return;
        }
        reply_event_detail(&record, sequence % PD_EVENT_ARCHIVE_COUNT);
        return;
    }
    if (parse_single_u32(p, &index) != 0 || index >= PD_EVENT_ARCHIVE_COUNT ||
        g_pd_acq.event[index].sequence >= g_pd_acq.event_sequence) {
        (void)reply("ERR usage: EVENT DETAIL index | EVENT DETAIL SEQ sequence\r\n");
        return;
    }
    record = g_pd_acq.event[index];
    reply_event_detail(&record, index);
}

static int prpd_current(pd_prpd_summary_t *summary)
{
    u32 next = g_pd_acq.event_sequence;
    u32 first = next > PD_EVENT_ARCHIVE_COUNT ? next - PD_EVENT_ARCHIVE_COUNT : 0U;
    if (pd_acq_state() != PD_ACQ_IDLE || first == next) return XST_FAILURE;
    return pd_prpd_build(first, next, summary);
}

static void reply_prpd_summary(void)
{
    pd_prpd_summary_t summary;
    char out[448];
    if (prpd_current(&summary) != XST_SUCCESS) {
        (void)reply("ERR PRPD SUMMARY requires IDLE and retained events\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "PRPD seq=[%lu,%lu) packets=%lu bins=%u ch0=n%lu/pos%lu/neg%lu/qmean%lu/qmax%lu/ph%lu ch1=n%lu/pos%lu/neg%lu/qmean%lu/qmax%lu/ph%lu ch2=n%lu/pos%lu/neg%lu/qmean%lu/qmax%lu/ph%lu ch3=n%lu/pos%lu/neg%lu/qmean%lu/qmax%lu/ph%lu\r\n",
        (unsigned long)summary.event_sequence_first,
        (unsigned long)summary.event_sequence_next,
        (unsigned long)summary.packet_count, PD_PRPD_PHASE_BINS,
        (unsigned long)summary.channel[0].peak_count,
        (unsigned long)summary.channel[0].positive_count,
        (unsigned long)summary.channel[0].negative_count,
        (unsigned long)summary.channel[0].q_abs_mean,
        (unsigned long)summary.channel[0].q_abs_max,
        (unsigned long)summary.channel[0].phase_at_max,
        (unsigned long)summary.channel[1].peak_count,
        (unsigned long)summary.channel[1].positive_count,
        (unsigned long)summary.channel[1].negative_count,
        (unsigned long)summary.channel[1].q_abs_mean,
        (unsigned long)summary.channel[1].q_abs_max,
        (unsigned long)summary.channel[1].phase_at_max,
        (unsigned long)summary.channel[2].peak_count,
        (unsigned long)summary.channel[2].positive_count,
        (unsigned long)summary.channel[2].negative_count,
        (unsigned long)summary.channel[2].q_abs_mean,
        (unsigned long)summary.channel[2].q_abs_max,
        (unsigned long)summary.channel[2].phase_at_max,
        (unsigned long)summary.channel[3].peak_count,
        (unsigned long)summary.channel[3].positive_count,
        (unsigned long)summary.channel[3].negative_count,
        (unsigned long)summary.channel[3].q_abs_mean,
        (unsigned long)summary.channel[3].q_abs_max,
        (unsigned long)summary.channel[3].phase_at_max);
    (void)reply(out);
}

static void start_prpd_bins(char *line)
{
    char *p = line + 10; /* "PRPD BINS " */
    pd_prpd_summary_t summary;
    u32 channel, first_bin, count, index;
    char out[192];
    int written;

    if (read_u32(&p, &channel) != 0 || read_u32(&p, &first_bin) != 0) {
        (void)reply("ERR usage: PRPD BINS channel first_bin [count]\r\n");
        return;
    }
    if (channel >= 4U || first_bin >= PD_PRPD_PHASE_BINS) {
        (void)reply("ERR PRPD BINS channel must be 0..3 and first_bin must be 0..63\r\n");
        return;
    }
    while (*p == ' ') ++p;
    if (*p == 0) count = 8U;
    else if (read_u32(&p, &count) != 0 || count == 0U || count > 8U) {
        (void)reply("ERR PRPD BINS count must be 1..8\r\n");
        return;
    }
    while (*p == ' ') ++p;
    if (*p != 0 || first_bin + count > PD_PRPD_PHASE_BINS ||
        prpd_current(&summary) != XST_SUCCESS) {
        (void)reply("ERR PRPD BINS requires IDLE, retained events, and bins inside 0..63\r\n");
        return;
    }
    written = snprintf(out, sizeof(out), "PRPD_BINS seq=[%lu,%lu) ch=%lu first=%lu count=%lu values=",
                       (unsigned long)summary.event_sequence_first,
                       (unsigned long)summary.event_sequence_next,
                       (unsigned long)channel, (unsigned long)first_bin,
                       (unsigned long)count);
    for (index = 0U; index < count && written > 0 && (u32)written < sizeof(out); ++index)
        written += snprintf(out + written, sizeof(out) - (u32)written, "%s%lu",
                            index == 0U ? "" : ",",
                            (unsigned long)summary.channel[channel].phase_bins[first_bin + index]);
    (void)snprintf(out + written, sizeof(out) - (u32)written, "\r\n");
    (void)reply(out);
}

static void reply_snapshot(u32 index)
{
    char out[192];
    pd_snapshot_record_t r;
    if (index >= PD_SNAP_ARCHIVE_COUNT ||
        g_pd_acq.snapshot[index].sequence >= g_pd_acq.snapshot_sequence) {
        (void)reply("ERR SNAP index has no valid record\r\n");
        return;
    }
    r = g_pd_acq.snapshot[index];
    (void)snprintf(out, sizeof(out),
        "SNAP index=%lu seq=%lu hw_slot=%lu src=%08lx dst=%08lx bytes=%lu flags=%08lx\r\n",
        (unsigned long)index, (unsigned long)r.sequence, (unsigned long)r.source_slot,
        (unsigned long)r.source_addr, (unsigned long)r.archive_addr,
        (unsigned long)r.bytes, (unsigned long)r.flags);
    (void)reply(out);
}

static void reply_catalog(void)
{
    char out[176];
    u32 event_next = g_pd_acq.event_sequence;
    u32 snap_next = g_pd_acq.snapshot_sequence;
    u32 event_first = event_next > PD_EVENT_ARCHIVE_COUNT ?
                      event_next - PD_EVENT_ARCHIVE_COUNT : 0U;
    u32 snap_first = snap_next > PD_SNAP_ARCHIVE_COUNT ?
                     snap_next - PD_SNAP_ARCHIVE_COUNT : 0U;
    (void)snprintf(out, sizeof(out),
        "CATALOG event_seq=[%lu,%lu) slots=%u snap_seq=[%lu,%lu) slots=%u state=%lu\r\n",
        (unsigned long)event_first, (unsigned long)event_next, PD_EVENT_ARCHIVE_COUNT,
        (unsigned long)snap_first, (unsigned long)snap_next, PD_SNAP_ARCHIVE_COUNT,
        (unsigned long)pd_acq_state());
    (void)reply(out);
}

static void reply_event_sequence(u32 sequence)
{
    char out[176];
    pd_event_record_t r;
    if (pd_acq_get_event_by_sequence(sequence, &r) != XST_SUCCESS) {
        (void)reply("ERR EVENT sequence is outside the retained archive window\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "EVENT seq=%lu index=%lu addr=%08lx bytes=%lu peaks=%lu cycles=%lu\r\n",
        (unsigned long)sequence, (unsigned long)(sequence % PD_EVENT_ARCHIVE_COUNT),
        (unsigned long)r.ddr_addr, (unsigned long)r.bytes,
        (unsigned long)r.peak_words, (unsigned long)r.cycle_words);
    (void)reply(out);
}

static void reply_snapshot_sequence(u32 sequence)
{
    char out[208];
    pd_snapshot_record_t r;
    if (pd_acq_get_snapshot_by_sequence(sequence, &r) != XST_SUCCESS) {
        (void)reply("ERR SNAP sequence is outside the retained archive window\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "SNAP seq=%lu index=%lu hw_slot=%lu src=%08lx dst=%08lx bytes=%lu flags=%08lx\r\n",
        (unsigned long)sequence, (unsigned long)(sequence % PD_SNAP_ARCHIVE_COUNT),
        (unsigned long)r.source_slot, (unsigned long)r.source_addr,
        (unsigned long)r.archive_addr, (unsigned long)r.bytes,
        (unsigned long)r.flags);
    (void)reply(out);
}

static void reply_snapshot_analysis(const pd_snapshot_record_t *record, u32 index)
{
    char out[320];
    pd_snapshot_stats_t stats;

    Xil_DCacheInvalidateRange((UINTPTR)record->archive_addr, record->bytes);
    if (pd_snapshot_measure((const void *)(UINTPTR)record->archive_addr,
                            record->bytes, &stats) != XST_SUCCESS) {
        (void)reply("ERR ANALYZE invalid snapshot layout\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "ANALYZE SNAP index=%lu seq=%lu bytes=%lu samples=%lu ch0=%u/%u/%lu ch1=%u/%u/%lu ch2=%u/%u/%lu ch3=%u/%u/%lu\r\n",
        (unsigned long)index, (unsigned long)record->sequence,
        (unsigned long)record->bytes, (unsigned long)stats.sample_count,
        stats.minimum[0], stats.maximum[0], (unsigned long)stats.mean[0],
        stats.minimum[1], stats.maximum[1], (unsigned long)stats.mean[1],
        stats.minimum[2], stats.maximum[2], (unsigned long)stats.mean[2],
        stats.minimum[3], stats.maximum[3], (unsigned long)stats.mean[3]);
    (void)reply(out);
}

static void start_snapshot_analysis(char *line)
{
    char *p = line + 8; /* "ANALYZE " */
    u32 index, sequence;
    pd_snapshot_record_t record;

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR ANALYZE is accepted only while idle\r\n");
        return;
    }
    if (strncmp(p, "SNAP ", 5) != 0) {
        (void)reply("ERR usage: ANALYZE SNAP index|SEQ sequence\r\n");
        return;
    }
    p += 5;
    if (strncmp(p, "SEQ ", 4) == 0) {
        p += 4;
        if (parse_single_u32(p, &sequence) != 0 ||
            pd_acq_get_snapshot_by_sequence(sequence, &record) != XST_SUCCESS) {
            (void)reply("ERR ANALYZE SNAP sequence is outside the retained archive window\r\n");
            return;
        }
        index = sequence % PD_SNAP_ARCHIVE_COUNT;
    } else {
        if (parse_single_u32(p, &index) != 0 || index >= PD_SNAP_ARCHIVE_COUNT ||
            g_pd_acq.snapshot[index].sequence >= g_pd_acq.snapshot_sequence) {
            (void)reply("ERR ANALYZE SNAP index has no valid record\r\n");
            return;
        }
        record = g_pd_acq.snapshot[index];
    }
    reply_snapshot_analysis(&record, index);
}

static void reply_fft_result(const pd_snapshot_record_t *record, u32 index,
                             u32 start_sample, const pd_spectrum_window_t *window,
                             const pd_spectrum_result_t *result)
{
    char out[480];
    (void)snprintf(out, sizeof(out),
        "FFT%s SNAP index=%lu seq=%lu start=%lu fs=%lu bin_hz=%lu event=%lu/%lu/%u/%lu ch0=%lu/%lu/%lu/%lu ch1=%lu/%lu/%lu/%lu ch2=%lu/%lu/%lu/%lu ch3=%lu/%lu/%lu/%lu\r\n",
        window == 0 ? "" : " AUTO",
        (unsigned long)index, (unsigned long)record->sequence,
        (unsigned long)start_sample, (unsigned long)result->sample_rate_hz,
        (unsigned long)(result->sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)(window == 0 ? 0U : window->peak_sample),
        (unsigned long)(window == 0 ? 0U : window->peak_channel),
        window == 0 ? 0U : window->raw_code,
        (unsigned long)(window == 0 ? 0U : window->delta_from_mid),
        (unsigned long)result->channel[0].peak_bin, (unsigned long)result->channel[0].peak_hz,
        (unsigned long)result->channel[0].amplitude_code, (unsigned long)result->channel[0].dc_code,
        (unsigned long)result->channel[1].peak_bin, (unsigned long)result->channel[1].peak_hz,
        (unsigned long)result->channel[1].amplitude_code, (unsigned long)result->channel[1].dc_code,
        (unsigned long)result->channel[2].peak_bin, (unsigned long)result->channel[2].peak_hz,
        (unsigned long)result->channel[2].amplitude_code, (unsigned long)result->channel[2].dc_code,
        (unsigned long)result->channel[3].peak_bin, (unsigned long)result->channel[3].peak_hz,
        (unsigned long)result->channel[3].amplitude_code, (unsigned long)result->channel[3].dc_code);
    (void)reply(out);
}

static void start_fft(char *line)
{
    char *p = line + 4; /* "FFT " */
    u32 index, sequence, start_sample = 0U, auto_window = 0U;
    pd_snapshot_record_t record;
    pd_spectrum_result_t result;
    pd_spectrum_window_t window;

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR FFT is accepted only while idle\r\n");
        return;
    }
    if (strcmp(p, "CONFIG") == 0) {
        char out[112];
        (void)snprintf(out, sizeof(out), "FFT_CONFIG points=%u sample_rate=%lu bin_hz=%lu window=HANN dc=MEAN\r\n",
            PD_FFT_POINTS, (unsigned long)s_fft_sample_rate_hz,
            (unsigned long)(s_fft_sample_rate_hz / PD_FFT_POINTS));
        (void)reply(out);
        return;
    }
    if (strcmp(p, "SELFTEST") == 0) {
        if (pd_spectrum_self_test(&result) != XST_SUCCESS)
            (void)reply("ERR FFT_SELFTEST failed\r\n");
        else
            (void)reply("FFT_SELFTEST_PASS bins=37/83/151/255 amplitude=1024(+/- tolerance)\r\n");
        return;
    }
    if (strncmp(p, "AUTO ", 5) == 0) {
        auto_window = 1U;
        p += 5;
    }
    if (strncmp(p, "SNAP ", 5) != 0) {
        (void)reply("ERR usage: FFT [AUTO] SNAP index|SEQ sequence [start_sample]\r\n");
        return;
    }
    p += 5;
    if (strncmp(p, "SEQ ", 4) == 0) {
        p += 4;
        if ((auto_window ? parse_single_u32(p, &sequence) :
                            parse_one_or_two_u32(p, &sequence, &start_sample)) != 0 ||
            pd_acq_get_snapshot_by_sequence(sequence, &record) != XST_SUCCESS) {
            (void)reply("ERR FFT SNAP sequence is outside the retained archive window\r\n");
            return;
        }
        index = sequence % PD_SNAP_ARCHIVE_COUNT;
    } else {
        if ((auto_window ? parse_single_u32(p, &index) :
                            parse_one_or_two_u32(p, &index, &start_sample)) != 0 || index >= PD_SNAP_ARCHIVE_COUNT ||
            g_pd_acq.snapshot[index].sequence >= g_pd_acq.snapshot_sequence) {
            (void)reply("ERR FFT SNAP index has no valid record\r\n");
            return;
        }
        record = g_pd_acq.snapshot[index];
    }
    if (auto_window != 0U) {
        if (pd_spectrum_find_peak_window((const void *)(UINTPTR)record.archive_addr,
                                         record.bytes, &window) != XST_SUCCESS) {
            (void)reply("ERR FFT AUTO cannot select a complete 1024-sample window\r\n");
            return;
        }
        start_sample = window.start_sample;
    }
    if ((record.bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U ||
        start_sample > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE ||
        PD_FFT_POINTS > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE - start_sample) {
        (void)reply("ERR FFT range requires 1024 complete samples inside SNAP\r\n");
        return;
    }
    Xil_DCacheInvalidateRange((UINTPTR)record.archive_addr +
                              start_sample * PD_SNAPSHOT_BYTES_PER_SAMPLE,
                              PD_FFT_POINTS * PD_SNAPSHOT_BYTES_PER_SAMPLE);
    if (pd_spectrum_analyze((const void *)(UINTPTR)record.archive_addr, record.bytes,
                            start_sample, s_fft_sample_rate_hz, &result) != XST_SUCCESS) {
        (void)reply("ERR FFT range requires 1024 complete samples inside SNAP\r\n");
        return;
    }
    reply_fft_result(&record, index, start_sample,
                     auto_window != 0U ? &window : 0, &result);
}

static void reply_analysis_record(const char *prefix, const pd_analysis_record_t *record)
{
    char out[512];
    const pd_spectrum_result_t *result = &record->spectrum;
    const pd_spectrum_window_t *window = &record->window;
    (void)snprintf(out, sizeof(out),
        "%s index=%lu analysis=%lu snap_seq=%lu snap_index=%lu start=%lu mode=%s fs=%lu bin_hz=%lu event=%lu/%lu/%u/%lu ch0=%lu/%lu/%lu/%lu ch1=%lu/%lu/%lu/%lu ch2=%lu/%lu/%lu/%lu ch3=%lu/%lu/%lu/%lu\r\n",
        prefix, (unsigned long)(record->analysis_sequence % PD_ANALYSIS_ARCHIVE_COUNT),
        (unsigned long)record->analysis_sequence,
        (unsigned long)record->snapshot_sequence, (unsigned long)record->snapshot_index,
        (unsigned long)result->start_sample,
        record->window_auto != 0U ? "AUTO" : "FIXED",
        (unsigned long)result->sample_rate_hz,
        (unsigned long)(result->sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)(record->window_auto != 0U ? window->peak_sample : 0U),
        (unsigned long)(record->window_auto != 0U ? window->peak_channel : 0U),
        record->window_auto != 0U ? window->raw_code : 0U,
        (unsigned long)(record->window_auto != 0U ? window->delta_from_mid : 0U),
        (unsigned long)result->channel[0].peak_bin, (unsigned long)result->channel[0].peak_hz,
        (unsigned long)result->channel[0].amplitude_code, (unsigned long)result->channel[0].dc_code,
        (unsigned long)result->channel[1].peak_bin, (unsigned long)result->channel[1].peak_hz,
        (unsigned long)result->channel[1].amplitude_code, (unsigned long)result->channel[1].dc_code,
        (unsigned long)result->channel[2].peak_bin, (unsigned long)result->channel[2].peak_hz,
        (unsigned long)result->channel[2].amplitude_code, (unsigned long)result->channel[2].dc_code,
        (unsigned long)result->channel[3].peak_bin, (unsigned long)result->channel[3].peak_hz,
        (unsigned long)result->channel[3].amplitude_code, (unsigned long)result->channel[3].dc_code);
    (void)reply(out);
}

static int parse_snapshot_selector(char *p, u32 auto_window, u32 *index,
                                   u32 *start_sample, pd_snapshot_record_t *record)
{
    u32 sequence;
    if (strncmp(p, "SEQ ", 4) == 0) {
        p += 4;
        if ((auto_window != 0U ? parse_single_u32(p, &sequence) :
                                 parse_one_or_two_u32(p, &sequence, start_sample)) != 0 ||
            pd_acq_get_snapshot_by_sequence(sequence, record) != XST_SUCCESS)
            return -1;
        *index = sequence % PD_SNAP_ARCHIVE_COUNT;
        return 0;
    }
    if ((auto_window != 0U ? parse_single_u32(p, index) :
                             parse_one_or_two_u32(p, index, start_sample)) != 0 ||
        *index >= PD_SNAP_ARCHIVE_COUNT ||
        g_pd_acq.snapshot[*index].sequence >= g_pd_acq.snapshot_sequence)
        return -1;
    *record = g_pd_acq.snapshot[*index];
    return 0;
}

/* SPECTRUM saves a reproducible FFT result; existing FFT remains a transient probe. */
static void start_spectrum(char *line)
{
    char *p = line + 9; /* "SPECTRUM " */
    u32 index, start_sample = 0U, auto_window = 0U;
    pd_snapshot_record_t snapshot;
    pd_analysis_record_t analysis;

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR SPECTRUM is accepted only while idle\r\n");
        return;
    }
    if (strncmp(p, "AUTO ", 5) == 0) {
        auto_window = 1U;
        p += 5;
    }
    if (strncmp(p, "SNAP ", 5) != 0) {
        (void)reply("ERR usage: SPECTRUM [AUTO] SNAP index|SEQ sequence [start_sample]\r\n");
        return;
    }
    p += 5;
    if (parse_snapshot_selector(p, auto_window, &index, &start_sample, &snapshot) != 0) {
        (void)reply("ERR SPECTRUM SNAP is outside the retained archive window\r\n");
        return;
    }
    if (pd_analysis_run(&snapshot, index, auto_window, start_sample,
                        s_fft_sample_rate_hz, &analysis) != XST_SUCCESS) {
        (void)reply("ERR SPECTRUM needs a complete 1024-sample window inside SNAP\r\n");
        return;
    }
    reply_analysis_record("SPECTRUM", &analysis);
}

static void start_analysis_query(char *line)
{
    char *p = line + 9; /* "ANALYSIS " */
    pd_analysis_record_t analysis;
    u32 index, sequence, first, next;
    char out[112];

    if (strcmp(p, "CATALOG") == 0) {
        next = pd_analysis_next_sequence();
        first = next > PD_ANALYSIS_ARCHIVE_COUNT ? next - PD_ANALYSIS_ARCHIVE_COUNT : 0U;
        (void)snprintf(out, sizeof(out),
            "ANALYSIS_CATALOG seq=[%lu,%lu) slots=%u\r\n",
            (unsigned long)first, (unsigned long)next, PD_ANALYSIS_ARCHIVE_COUNT);
        (void)reply(out);
        return;
    }
    if (strncmp(p, "SNAP SEQ ", 9) == 0) {
        if (parse_single_u32(p + 9, &sequence) != 0 ||
            pd_analysis_get_by_snapshot_sequence(sequence, &analysis) != XST_SUCCESS) {
            (void)reply("ERR ANALYSIS has no retained record for SNAP sequence\r\n");
            return;
        }
    } else {
        if (parse_single_u32(p, &index) != 0 ||
            pd_analysis_get(index, &analysis) != XST_SUCCESS) {
            (void)reply("ERR ANALYSIS index has no valid record\r\n");
            return;
        }
    }
    reply_analysis_record("ANALYSIS", &analysis);
}

static void reply_sweep_header(const pd_analysis_sweep_t *sweep)
{
    char out[352];
    (void)snprintf(out, sizeof(out),
        "SWEEP index=%lu sweep=%lu snap_seq=%lu snap_index=%lu event=%lu/%lu/%u/%lu starts=%lu,%lu,%lu,%lu,%lu\r\n",
        (unsigned long)(sweep->sweep_sequence % PD_ANALYSIS_ARCHIVE_COUNT),
        (unsigned long)sweep->sweep_sequence,
        (unsigned long)sweep->snapshot_sequence, (unsigned long)sweep->snapshot_index,
        (unsigned long)sweep->event.peak_sample,
        (unsigned long)sweep->event.peak_channel, sweep->event.raw_code,
        (unsigned long)sweep->event.delta_from_mid,
        (unsigned long)sweep->start_sample[0], (unsigned long)sweep->start_sample[1],
        (unsigned long)sweep->start_sample[2], (unsigned long)sweep->start_sample[3],
        (unsigned long)sweep->start_sample[4]);
    (void)reply(out);
}

static void reply_sweep_window(const pd_analysis_sweep_t *sweep, u32 ordinal)
{
    const pd_spectrum_result_t *result = &sweep->spectrum[ordinal];
    char out[416];
    (void)snprintf(out, sizeof(out),
        "SWEEP_WINDOW index=%lu sweep=%lu ordinal=%lu start=%lu fs=%lu bin_hz=%lu ch0=%lu/%lu/%lu/%lu/p%llu ch1=%lu/%lu/%lu/%lu/p%llu ch2=%lu/%lu/%lu/%lu/p%llu ch3=%lu/%lu/%lu/%lu/p%llu\r\n",
        (unsigned long)(sweep->sweep_sequence % PD_ANALYSIS_ARCHIVE_COUNT),
        (unsigned long)sweep->sweep_sequence, (unsigned long)ordinal,
        (unsigned long)result->start_sample, (unsigned long)result->sample_rate_hz,
        (unsigned long)(result->sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)result->channel[0].peak_bin, (unsigned long)result->channel[0].peak_hz,
        (unsigned long)result->channel[0].amplitude_code, (unsigned long)result->channel[0].dc_code,
        (unsigned long long)result->channel[0].band_power,
        (unsigned long)result->channel[1].peak_bin, (unsigned long)result->channel[1].peak_hz,
        (unsigned long)result->channel[1].amplitude_code, (unsigned long)result->channel[1].dc_code,
        (unsigned long long)result->channel[1].band_power,
        (unsigned long)result->channel[2].peak_bin, (unsigned long)result->channel[2].peak_hz,
        (unsigned long)result->channel[2].amplitude_code, (unsigned long)result->channel[2].dc_code,
        (unsigned long long)result->channel[2].band_power,
        (unsigned long)result->channel[3].peak_bin, (unsigned long)result->channel[3].peak_hz,
        (unsigned long)result->channel[3].amplitude_code, (unsigned long)result->channel[3].dc_code,
        (unsigned long long)result->channel[3].band_power);
    (void)reply(out);
}

static void reply_sweep_feature(const pd_analysis_sweep_t *sweep)
{
    pd_sweep_feature_t feature;
    char out[640];
    if (pd_analysis_sweep_feature(sweep, &feature) != XST_SUCCESS) {
        (void)reply("ERR FEATURE has no valid retained sweep\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "FEATURE index=%lu sweep=%lu snap_seq=%lu event=%lu/%lu/%u/%lu ch0=%llu/%llu/%llu/%ld/%ld ch1=%llu/%llu/%llu/%ld/%ld ch2=%llu/%llu/%llu/%ld/%ld ch3=%llu/%llu/%llu/%ld/%ld\r\n",
        (unsigned long)(feature.sweep_sequence % PD_ANALYSIS_ARCHIVE_COUNT),
        (unsigned long)feature.sweep_sequence, (unsigned long)feature.snapshot_sequence,
        (unsigned long)feature.event.peak_sample,
        (unsigned long)feature.event.peak_channel, feature.event.raw_code,
        (unsigned long)feature.event.delta_from_mid,
        (unsigned long long)feature.channel[0].pre_band_power,
        (unsigned long long)feature.channel[0].event_band_power,
        (unsigned long long)feature.channel[0].post_band_power,
        (long)feature.channel[0].event_delta_permille,
        (long)feature.channel[0].recovery_permille,
        (unsigned long long)feature.channel[1].pre_band_power,
        (unsigned long long)feature.channel[1].event_band_power,
        (unsigned long long)feature.channel[1].post_band_power,
        (long)feature.channel[1].event_delta_permille,
        (long)feature.channel[1].recovery_permille,
        (unsigned long long)feature.channel[2].pre_band_power,
        (unsigned long long)feature.channel[2].event_band_power,
        (unsigned long long)feature.channel[2].post_band_power,
        (long)feature.channel[2].event_delta_permille,
        (long)feature.channel[2].recovery_permille,
        (unsigned long long)feature.channel[3].pre_band_power,
        (unsigned long long)feature.channel[3].event_band_power,
        (unsigned long long)feature.channel[3].post_band_power,
        (long)feature.channel[3].event_delta_permille,
        (long)feature.channel[3].recovery_permille);
    (void)reply(out);
}

static void start_feature(char *line)
{
    char *p = line + 8; /* "FEATURE " */
    pd_analysis_sweep_t sweep;
    u32 index, sequence;

    if (strncmp(p, "SWEEP ", 6) != 0) {
        (void)reply("ERR usage: FEATURE SWEEP index | FEATURE SWEEP SNAP SEQ sequence\r\n");
        return;
    }
    p += 6;
    if (strncmp(p, "SNAP SEQ ", 9) == 0) {
        if (parse_single_u32(p + 9, &sequence) != 0 ||
            pd_analysis_sweep_get_by_snapshot_sequence(sequence, &sweep) != XST_SUCCESS) {
            (void)reply("ERR FEATURE has no retained sweep for SNAP sequence\r\n");
            return;
        }
    } else if (parse_single_u32(p, &index) != 0 ||
               pd_analysis_sweep_get(index, &sweep) != XST_SUCCESS) {
        (void)reply("ERR FEATURE sweep index has no valid record\r\n");
        return;
    }
    reply_sweep_feature(&sweep);
}

static void reply_alert_config(void)
{
    char out[112];
    (void)snprintf(out, sizeof(out),
        "ALERT_CONFIG enabled_mask=0x%lx delta_threshold_permille=%ld enabled=%s\r\n",
        (unsigned long)s_alert_channel_mask, (long)s_alert_delta_permille,
        s_alert_delta_permille > 0 ? "yes" : "no");
    (void)reply(out);
}

static void reply_sweep_alert(const pd_analysis_sweep_t *sweep)
{
    pd_sweep_alert_t alert;
    char out[352];
    if (pd_analysis_sweep_alert(sweep, s_alert_channel_mask,
                                s_alert_delta_permille, &alert) != XST_SUCCESS) {
        (void)reply("ERR ALERT has no valid retained sweep\r\n");
        return;
    }
    (void)snprintf(out, sizeof(out),
        "ALERT index=%lu sweep=%lu snap_seq=%lu enabled_mask=0x%lx threshold=%ld hit_mask=0x%lx delta=%ld,%ld,%ld,%ld recovery=%ld,%ld,%ld,%ld\r\n",
        (unsigned long)(alert.feature.sweep_sequence % PD_ANALYSIS_ARCHIVE_COUNT),
        (unsigned long)alert.feature.sweep_sequence,
        (unsigned long)alert.feature.snapshot_sequence,
        (unsigned long)alert.selected_mask,
        (long)alert.delta_threshold_permille, (unsigned long)alert.hit_mask,
        (long)alert.feature.channel[0].event_delta_permille,
        (long)alert.feature.channel[1].event_delta_permille,
        (long)alert.feature.channel[2].event_delta_permille,
        (long)alert.feature.channel[3].event_delta_permille,
        (long)alert.feature.channel[0].recovery_permille,
        (long)alert.feature.channel[1].recovery_permille,
        (long)alert.feature.channel[2].recovery_permille,
        (long)alert.feature.channel[3].recovery_permille);
    (void)reply(out);
}

static void start_alert(char *line)
{
    char *p = line + 6; /* "ALERT " */
    pd_analysis_sweep_t sweep;
    u32 index, sequence;

    if (strcmp(p, "CONFIG") == 0) {
        reply_alert_config();
        return;
    }
    if (strncmp(p, "SWEEP ", 6) != 0) {
        (void)reply("ERR usage: ALERT CONFIG | ALERT SWEEP index | ALERT SWEEP SNAP SEQ sequence\r\n");
        return;
    }
    p += 6;
    if (strncmp(p, "SNAP SEQ ", 9) == 0) {
        if (parse_single_u32(p + 9, &sequence) != 0 ||
            pd_analysis_sweep_get_by_snapshot_sequence(sequence, &sweep) != XST_SUCCESS) {
            (void)reply("ERR ALERT has no retained sweep for SNAP sequence\r\n");
            return;
        }
    } else if (parse_single_u32(p, &index) != 0 ||
               pd_analysis_sweep_get(index, &sweep) != XST_SUCCESS) {
        (void)reply("ERR ALERT sweep index has no valid record\r\n");
        return;
    }
    reply_sweep_alert(&sweep);
}

/*
 * Offline batch operation: one five-window sweep is created for each snapshot
 * retained by the PS DDR archive.  It deliberately runs only while IDLE so the
 * archive metadata cannot rotate underneath the sequence walk.  The four-slot
 * sweep ring is exactly the same size as the snapshot archive, therefore a
 * complete batch remains queryable through the existing SWEEP/FEATURE/ALERT
 * commands until later sweep activity overwrites it.
 */
static void start_batch(char *line)
{
    u32 first, next, sequence, sweep_first, sweep_next;
    u32 processed = 0U, hit_union = 0U, hit_records = 0U;
    pd_snapshot_record_t snapshot;
    pd_analysis_sweep_t sweep;
    pd_sweep_alert_t alert;
    char out[240];

    if (strcmp(line, "BATCH AUTO") != 0) {
        (void)reply("ERR usage: BATCH AUTO\r\n");
        return;
    }
    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR BATCH is accepted only while idle\r\n");
        return;
    }

    next = g_pd_acq.snapshot_sequence;
    first = next > PD_SNAP_ARCHIVE_COUNT ? next - PD_SNAP_ARCHIVE_COUNT : 0U;
    if (first == next) {
        (void)reply("ERR BATCH has no retained snapshots\r\n");
        return;
    }
    sweep_first = pd_analysis_sweep_next_sequence();
    for (sequence = first; sequence < next; ++sequence) {
        if (pd_acq_get_snapshot_by_sequence(sequence, &snapshot) != XST_SUCCESS ||
            pd_analysis_sweep_run(&snapshot, sequence % PD_SNAP_ARCHIVE_COUNT,
                                  s_fft_sample_rate_hz, &sweep) != XST_SUCCESS ||
            pd_analysis_sweep_alert(&sweep, s_alert_channel_mask,
                                    s_alert_delta_permille, &alert) != XST_SUCCESS) {
            (void)snprintf(out, sizeof(out),
                "ERR BATCH failed while processing SNAP sequence %lu\r\n",
                (unsigned long)sequence);
            (void)reply(out);
            return;
        }
        hit_union |= alert.hit_mask;
        if (alert.hit_mask != 0U) ++hit_records;
        ++processed;
    }
    sweep_next = pd_analysis_sweep_next_sequence();
    (void)snprintf(out, sizeof(out),
        "BATCH snap_seq=[%lu,%lu) processed=%lu sweep_seq=[%lu,%lu) enabled_mask=0x%lx threshold=%ld hit_union=0x%lx hit_records=%lu\r\n",
        (unsigned long)first, (unsigned long)next, (unsigned long)processed,
        (unsigned long)sweep_first, (unsigned long)sweep_next,
        (unsigned long)s_alert_channel_mask, (long)s_alert_delta_permille,
        (unsigned long)hit_union, (unsigned long)hit_records);
    (void)reply(out);
}

static void start_sweep(char *line)
{
    char *p = line + 6; /* "SWEEP " */
    pd_snapshot_record_t snapshot;
    pd_analysis_sweep_t sweep;
    u32 index, sequence, ordinal, next, first;
    char out[112];

    if (strncmp(p, "AUTO SNAP ", 10) == 0) {
        if (pd_acq_state() != PD_ACQ_IDLE) {
            (void)reply("ERR SWEEP is accepted only while idle\r\n");
            return;
        }
        p += 10;
        if (parse_snapshot_selector(p, 1U, &index, &ordinal, &snapshot) != 0) {
            (void)reply("ERR SWEEP SNAP is outside the retained archive window\r\n");
            return;
        }
        if (pd_analysis_sweep_run(&snapshot, index, s_fft_sample_rate_hz,
                                  &sweep) != XST_SUCCESS) {
            (void)reply("ERR SWEEP needs five complete 1024-sample windows inside SNAP\r\n");
            return;
        }
        reply_sweep_header(&sweep);
        return;
    }
    if (strcmp(p, "CATALOG") == 0) {
        next = pd_analysis_sweep_next_sequence();
        first = next > PD_ANALYSIS_ARCHIVE_COUNT ? next - PD_ANALYSIS_ARCHIVE_COUNT : 0U;
        (void)snprintf(out, sizeof(out), "SWEEP_CATALOG seq=[%lu,%lu) slots=%u windows=%u\r\n",
            (unsigned long)first, (unsigned long)next, PD_ANALYSIS_ARCHIVE_COUNT,
            PD_ANALYSIS_SWEEP_WINDOWS);
        (void)reply(out);
        return;
    }
    if (strncmp(p, "SNAP SEQ ", 9) == 0) {
        if (parse_single_u32(p + 9, &sequence) != 0 ||
            pd_analysis_sweep_get_by_snapshot_sequence(sequence, &sweep) != XST_SUCCESS) {
            (void)reply("ERR SWEEP has no retained record for SNAP sequence\r\n");
            return;
        }
        reply_sweep_header(&sweep);
        return;
    }
    if (strncmp(p, "INDEX ", 6) == 0) {
        if (parse_single_u32(p + 6, &index) != 0 ||
            pd_analysis_sweep_get(index, &sweep) != XST_SUCCESS) {
            (void)reply("ERR SWEEP index has no valid record\r\n");
            return;
        }
        reply_sweep_header(&sweep);
        return;
    }
    if (strncmp(p, "WINDOW ", 7) == 0) {
        if (parse_one_or_two_u32(p + 7, &index, &ordinal) != 0 ||
            index >= PD_ANALYSIS_ARCHIVE_COUNT || ordinal >= PD_ANALYSIS_SWEEP_WINDOWS ||
            pd_analysis_sweep_get(index, &sweep) != XST_SUCCESS) {
            (void)reply("ERR usage: SWEEP WINDOW index ordinal(0..4)\r\n");
            return;
        }
        reply_sweep_window(&sweep, ordinal);
        return;
    }
    (void)reply("ERR usage: SWEEP AUTO SNAP index|SEQ n | CATALOG | INDEX n | WINDOW n 0..4 | SNAP SEQ n\r\n");
}

/*
 * Stream one fresh oscilloscope frame from the PL raw-data DDR ring.
 * The ring writer advances in 24-byte blocks (four 48-bit samples), so the
 * source window is deliberately kept 24 KiB behind RING_WR_PTR and aligned
 * to a complete block.  This is a live view, not an archived snapshot.
 */
static void start_scope_frame(void)
{
    u32 ring_base, ring_size, wr_off, bytes, start, first, sum;
    char out[160];

    if (!s_scope.enabled) {
        (void)reply("ERR SCOPE is off; send SCOPE ON [samples] first\r\n");
        return;
    }
    if (pd_acq_state() == PD_ACQ_IDLE) {
        (void)reply("ERR SCOPE requires a running acquisition\r\n");
        return;
    }
    if (s_transfer.active) {
        (void)reply("ERR SCOPE transfer is still active\r\n");
        return;
    }

    ring_base = ddr_read(PD_DDR_RING_BASE);
    ring_size = ddr_read(PD_DDR_RING_SIZE);
    wr_off = ddr_read(PD_DDR_RING_WR_PTR);
    bytes = s_scope.samples * PD_SCOPE_SAMPLE_BYTES;
    if (ring_base == 0U || ring_size < bytes + PD_SCOPE_SAFETY_BYTES + 24U ||
        wr_off >= ring_size) {
        (void)reply("ERR SCOPE raw DDR ring is not ready\r\n");
        return;
    }

    start = (wr_off + ring_size - PD_SCOPE_SAFETY_BYTES - bytes) % ring_size;
    start -= start % 24U;
    first = bytes;
    if (start + first > ring_size) first = ring_size - start;
    Xil_DCacheInvalidateRange((UINTPTR)ring_base + start, first);
    memcpy(s_scope_buffer, (const void *)(UINTPTR)(ring_base + start), first);
    if (first < bytes) {
        Xil_DCacheInvalidateRange((UINTPTR)ring_base, bytes - first);
        memcpy(s_scope_buffer + first, (const void *)(UINTPTR)ring_base, bytes - first);
    }

    sum = crc32(s_scope_buffer, bytes);
    (void)snprintf(out, sizeof(out),
        "SCOPE V1 seq=%lu samples=%lu bytes=%lu fs=26000000 crc32=%08lx\r\n",
        (unsigned long)s_scope.sequence++, (unsigned long)s_scope.samples,
        (unsigned long)bytes, (unsigned long)sum);
    if (reply(out) != 0) return;
    s_transfer.addr = (UINTPTR)s_scope_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    /* This buffer was just written by the CPU; invalidating it would discard
     * dirty cache lines before lwIP copies the payload. */
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/* Copy one retained PL event packet into a PS-owned transfer buffer.  The
 * peak phase is measured against sync_in by pd_feature_core, not inferred
 * from the unrelated 1024-sample raw DDR oscilloscope window.  Sampling the
 * peaks evenly keeps the network/UI load bounded without distorting the
 * packet's phase coverage toward its beginning. */
static void start_scope_peaks(void)
{
    u32 next, first, sequence, index, total, stride, selected, bytes;
    u32 win[4], lock_mask = 0U, channel;
    pd_event_record_t record;
    const u64 *words;
    char out[160];

    if (!s_scope.enabled) {
        (void)reply("ERR SCOPE is off; send SCOPE ON [samples] first\r\n");
        return;
    }
    if (s_transfer.active) {
        (void)reply("ERR SCOPE transfer is still active\r\n");
        return;
    }
    next = g_pd_acq.event_sequence;
    first = next > PD_EVENT_ARCHIVE_COUNT ? next - PD_EVENT_ARCHIVE_COUNT : 0U;
    if (s_scope.peak_cursor < first) s_scope.peak_cursor = first;
    if (s_scope.peak_cursor >= next) {
        (void)snprintf(out, sizeof(out), "PEAKS NONE next=%lu\r\n", (unsigned long)next);
        (void)reply(out);
        return;
    }
    sequence = s_scope.peak_cursor++;
    if (pd_acq_get_event_by_sequence(sequence, &record) != XST_SUCCESS ||
        record.bytes == 0U || record.bytes > PD_EVENT_ARCHIVE_STRIDE ||
        (record.bytes & 7U) != 0U) {
        (void)snprintf(out, sizeof(out), "PEAKS SKIP seq=%lu\r\n", (unsigned long)sequence);
        (void)reply(out);
        return;
    }
    Xil_DCacheInvalidateRange((UINTPTR)record.ddr_addr, record.bytes);
    words = (const u64 *)(UINTPTR)record.ddr_addr;
    total = record.peak_words;
    stride = total > PD_SCOPE_MAX_PEAKS ?
             (total + PD_SCOPE_MAX_PEAKS - 1U) / PD_SCOPE_MAX_PEAKS : 1U;
    selected = 0U;
    total = 0U;
    for (index = 0U; index < record.bytes / 8U; ++index) {
        u64 word = words[index];
        if ((u32)(word >> 56) != 0U) continue;
        if ((total % stride) == 0U && selected < PD_SCOPE_MAX_PEAKS)
            s_scope_peak_buffer[selected++] = word;
        ++total;
    }
    bytes = selected * 8U;
    for (channel = 0U; channel < 4U; ++channel) {
        win[channel] = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_CFG0(channel)) >> 16;
        if (Xil_In32(PD_FEATURE_BASE + PD_FEATURE_STATUS(channel)) & 1U)
            lock_mask |= 1U << channel;
    }
    (void)snprintf(out, sizeof(out),
        "PEAKS V1 seq=%lu total=%lu count=%lu bytes=%lu crc32=%08lx wins=%lu,%lu,%lu,%lu lock=%lx\r\n",
        (unsigned long)sequence, (unsigned long)total, (unsigned long)selected,
        (unsigned long)bytes,
        (unsigned long)crc32((const u8 *)s_scope_peak_buffer, bytes),
        (unsigned long)win[0], (unsigned long)win[1],
        (unsigned long)win[2], (unsigned long)win[3],
        (unsigned long)lock_mask);
    if (reply(out) != 0) return;
    if (bytes == 0U) return;
    s_transfer.addr = (UINTPTR)s_scope_peak_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

static void reply_scope_phase(void)
{
    u32 win[4], lock_mask = 0U, channel;
    char out[128];
    for (channel = 0U; channel < 4U; ++channel) {
        win[channel] = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_CFG0(channel)) >> 16;
        if (Xil_In32(PD_FEATURE_BASE + PD_FEATURE_STATUS(channel)) & 1U)
            lock_mask |= 1U << channel;
    }
    (void)snprintf(out, sizeof(out),
        "SCOPE_PHASE wins=%lu,%lu,%lu,%lu lock=%lx\r\n",
        (unsigned long)win[0], (unsigned long)win[1],
        (unsigned long)win[2], (unsigned long)win[3],
        (unsigned long)lock_mask);
    (void)reply(out);
}

static void start_scope(char *line)
{
    char *p = line + 5;
    u32 samples;
    while (*p == ' ') ++p;
    if (strncmp(p, "OFF", 3) == 0 && p[3] == 0) {
        s_scope.enabled = 0U;
        (void)reply("OK SCOPE OFF\r\n");
        return;
    }
    if (strncmp(p, "NEXT", 4) == 0 && p[4] == 0) {
        start_scope_frame();
        return;
    }
    if (strncmp(p, "PEAKS", 5) == 0 && p[5] == 0) {
        start_scope_peaks();
        return;
    }
    if (strncmp(p, "PHASE", 5) == 0 && p[5] == 0) {
        reply_scope_phase();
        return;
    }
    if (strncmp(p, "ON", 2) != 0 || (p[2] != 0 && p[2] != ' ')) {
        (void)reply("ERR usage: SCOPE ON [samples] | NEXT | PEAKS | PHASE | OFF\r\n");
        return;
    }
    p += 2;
    while (*p == ' ') ++p;
    samples = *p == 0 ? PD_SCOPE_DEFAULT_SAMPLES : parse_u32(p, 0U);
    if (samples < 256U || samples > PD_SCOPE_MAX_SAMPLES || (samples % 4U) != 0U) {
        (void)reply("ERR SCOPE samples must be 256..2048 and divisible by 4\r\n");
        return;
    }
    s_scope.enabled = 1U;
    s_scope.samples = samples;
    s_scope.peak_cursor = g_pd_acq.event_sequence > 4U ?
                          g_pd_acq.event_sequence - 4U : 0U;
    {
        char out[96];
        (void)snprintf(out, sizeof(out),
            "OK SCOPE ON samples=%lu fs=26000000; send SCOPE NEXT\r\n",
            (unsigned long)s_scope.samples);
        (void)reply(out);
    }
}

static void start_get(char *line)
{
    char *p = line + 4;
    const char *kind;
    UINTPTR base;
    u32 index, sequence, offset, bytes, available, sum;
    pd_event_record_t event_record;
    pd_snapshot_record_t snapshot_record;
    char out[192];

    while (*p == ' ') ++p;
    /* EVENT is a compact archived record and may be read during START 0 for
       live PRPD display.  SNAP remains IDLE-only because it is multi-chunk. */
    if (pd_acq_state() != PD_ACQ_IDLE &&
        !(strncmp(p, "EVENT", 5) == 0 && p[5] == ' ')) {
        (void)reply("ERR GET SNAP requires idle while acquisition is running\r\n");
        return;
    }
    if (strncmp(p, "EVENT", 5) == 0 && p[5] == ' ') {
        kind = "EVENT";
        p += 5;
        while (*p == ' ') ++p;
        if (strncmp(p, "SEQ ", 4) == 0) {
            p += 3;
            if (read_u32(&p, &sequence) != 0 ||
                pd_acq_get_event_by_sequence(sequence, &event_record) != XST_SUCCESS) {
                (void)reply("ERR GET EVENT sequence is outside the retained archive window\r\n");
                return;
            }
            index = sequence % PD_EVENT_ARCHIVE_COUNT;
            base = (UINTPTR)event_record.ddr_addr;
            available = event_record.bytes;
        } else {
            if (read_u32(&p, &index) != 0 || index >= PD_EVENT_ARCHIVE_COUNT ||
                g_pd_acq.event[index].sequence >= g_pd_acq.event_sequence) {
                (void)reply("ERR GET EVENT has no valid record\r\n");
                return;
            }
            base = (UINTPTR)g_pd_acq.event[index].ddr_addr;
            available = g_pd_acq.event[index].bytes;
        }
    } else if (strncmp(p, "SNAP", 4) == 0 && p[4] == ' ') {
        kind = "SNAP";
        p += 4;
        while (*p == ' ') ++p;
        if (strncmp(p, "SEQ ", 4) == 0) {
            p += 3;
            if (read_u32(&p, &sequence) != 0 ||
                pd_acq_get_snapshot_by_sequence(sequence, &snapshot_record) != XST_SUCCESS) {
                (void)reply("ERR GET SNAP sequence is outside the retained archive window\r\n");
                return;
            }
            index = sequence % PD_SNAP_ARCHIVE_COUNT;
            base = (UINTPTR)snapshot_record.archive_addr;
            available = snapshot_record.bytes;
        } else {
            if (read_u32(&p, &index) != 0 || index >= PD_SNAP_ARCHIVE_COUNT ||
                g_pd_acq.snapshot[index].sequence >= g_pd_acq.snapshot_sequence) {
                (void)reply("ERR GET SNAP has no valid record\r\n");
                return;
            }
            base = (UINTPTR)g_pd_acq.snapshot[index].archive_addr;
            available = g_pd_acq.snapshot[index].bytes;
        }
    } else {
        (void)reply("ERR usage: GET EVENT|SNAP index offset bytes\r\n");
        return;
    }
    if (read_u32(&p, &offset) != 0 || read_u32(&p, &bytes) != 0) {
        (void)reply("ERR usage: GET EVENT|SNAP index offset bytes\r\n");
        return;
    }
    while (*p == ' ') ++p;
    if (*p != 0 || offset >= available || bytes == 0U || bytes > PD_TCP_GET_MAX_BYTES ||
        bytes > available - offset) {
        (void)reply("ERR GET range; bytes must be 1..16384 inside record\r\n");
        return;
    }
    Xil_DCacheInvalidateRange(base + offset, bytes);
    sum = crc32((const u8 *)(base + offset), bytes);
    (void)snprintf(out, sizeof(out),
        "DATA V2 kind=%s index=%lu offset=%lu bytes=%lu crc32=%08lx\r\n",
        kind, (unsigned long)index, (unsigned long)offset,
        (unsigned long)bytes, (unsigned long)sum);
    if (reply(out) != 0) return;
    s_transfer.addr = base;
    s_transfer.offset = offset;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 1U;
    s_transfer.active = 1U;
}

static void execute_command(char *line)
{
    u32 value;
    if (s_transfer.active) return; /* Never interleave text and raw payload. */
    if (strcmp(line, "HELP") == 0) {
        (void)reply("CMD: START [n] | STOP | STATUS | CONFIG | SCOPE ON [samples]|NEXT|PEAKS|PHASE|OFF | REPORT | SET LIMIT n | SET FFTFS hz | SET ALERT DELTA n|MASK n | ALERT CONFIG|SWEEP n|SNAP SEQ n | RECOVER | CATALOG | EVENT n|SEQ n|DETAIL n|DETAIL SEQ n | PRPD SUMMARY|BINS ch first [count] | SNAP n|SEQ n | ANALYZE SNAP n|SEQ n | FFT CONFIG|SELFTEST|SNAP n|SEQ n [start]|AUTO SNAP n|SEQ n | SPECTRUM [AUTO] SNAP n|SEQ n [start] | ANALYSIS n|SNAP SEQ n|CATALOG | SWEEP AUTO SNAP n|SEQ n|CATALOG|INDEX n|WINDOW n 0..4|SNAP SEQ n | FEATURE SWEEP n|SNAP SEQ n | BATCH AUTO | GET EVENT|SNAP n|SEQ n offset bytes | CLEAR\r\n");
    } else if (strncmp(line, "START", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        if (pd_acq_state() != PD_ACQ_IDLE) (void)reply("ERR already running\r\n");
        else if (pd_acq_start(parse_u32(line + 5, pd_acq_default_packet_limit())) == XST_SUCCESS)
            (void)reply("OK start accepted\r\n");
        else (void)reply("ERR start rejected\r\n");
    } else if (strcmp(line, "STOP") == 0) {
        if (pd_acq_state() == PD_ACQ_IDLE) (void)reply("OK already idle\r\n");
        else { pd_acq_request_stop(); (void)reply("OK stop requested\r\n"); }
    } else if (strcmp(line, "STATUS") == 0) {
        reply_status();
    } else if (strcmp(line, "CONFIG") == 0) {
        reply_config();
    } else if (strcmp(line, "REPORT") == 0) {
        reply_report();
    } else if (strncmp(line, "SET LIMIT ", 10) == 0) {
        if (parse_single_u32(line + 10, &value) != 0)
            (void)reply("ERR usage: SET LIMIT packets; 0 means continuous\r\n");
        else if (pd_acq_set_default_packet_limit(value) != XST_SUCCESS)
            (void)reply("ERR SET requires idle\r\n");
        else {
            char out[64];
            (void)snprintf(out, sizeof(out), "OK default_limit=%lu\r\n", (unsigned long)value);
            (void)reply(out);
        }
    } else if (strncmp(line, "SET FFTFS ", 10) == 0) {
        if (pd_acq_state() != PD_ACQ_IDLE || parse_single_u32(line + 10, &value) != 0 || value == 0U) {
            (void)reply("ERR SET FFTFS requires idle and a nonzero Hz value\r\n");
        } else {
            char out[80];
            s_fft_sample_rate_hz = value;
            (void)snprintf(out, sizeof(out), "OK fft_sample_rate=%lu\r\n", (unsigned long)value);
            (void)reply(out);
        }
    } else if (strncmp(line, "SET ALERT DELTA ", 16) == 0) {
        if (pd_acq_state() != PD_ACQ_IDLE || parse_single_u32(line + 16, &value) != 0 ||
            value > 0x7FFFFFFFU) {
            (void)reply("ERR SET ALERT DELTA requires idle and 0..2147483647 permille\r\n");
        } else {
            char out[80];
            s_alert_delta_permille = (s32)value;
            (void)snprintf(out, sizeof(out), "OK alert_delta_permille=%ld\r\n",
                           (long)s_alert_delta_permille);
            (void)reply(out);
        }
    } else if (strncmp(line, "SET ALERT MASK ", 15) == 0) {
        if (pd_acq_state() != PD_ACQ_IDLE || parse_single_u32(line + 15, &value) != 0 ||
            value > 0xFU) {
            (void)reply("ERR SET ALERT MASK requires idle and a 4-bit mask\r\n");
        } else {
            char out[64];
            s_alert_channel_mask = value;
            (void)snprintf(out, sizeof(out), "OK alert_mask=0x%lx\r\n",
                           (unsigned long)s_alert_channel_mask);
            (void)reply(out);
        }
    } else if (strcmp(line, "RECOVER") == 0) {
        if (pd_acq_state() != PD_ACQ_FAULT) {
            (void)reply("ERR RECOVER requires fault state\r\n");
        } else if (pd_acq_recover(&value) != XST_SUCCESS) {
            (void)reply("ERR RECOVER failed; inspect STATUS\r\n");
        } else {
            char out[112];
            (void)snprintf(out, sizeof(out),
                "OK RECOVER discarded_slots=%lu archives events=%lu snaps=%lu\r\n",
                (unsigned long)value, (unsigned long)g_pd_acq.event_sequence,
                (unsigned long)g_pd_acq.snapshot_sequence);
            (void)reply(out);
        }
    } else if (strcmp(line, "CATALOG") == 0) {
        reply_catalog();
    } else if (strncmp(line, "EVENT SEQ ", 10) == 0) {
        reply_event_sequence(parse_u32(line + 10, 0xFFFFFFFFU));
    } else if (strncmp(line, "EVENT DETAIL ", 13) == 0) {
        start_event_detail(line);
    } else if (strcmp(line, "PRPD SUMMARY") == 0) {
        reply_prpd_summary();
    } else if (strncmp(line, "PRPD BINS ", 10) == 0) {
        start_prpd_bins(line);
    } else if (strncmp(line, "SNAP SEQ ", 9) == 0) {
        reply_snapshot_sequence(parse_u32(line + 9, 0xFFFFFFFFU));
    } else if (strncmp(line, "EVENT ", 6) == 0) {
        reply_event(parse_u32(line + 6, PD_EVENT_ARCHIVE_COUNT));
    } else if (strncmp(line, "SNAP ", 5) == 0) {
        reply_snapshot(parse_u32(line + 5, PD_SNAP_ARCHIVE_COUNT));
    } else if (strncmp(line, "ANALYZE ", 8) == 0) {
        start_snapshot_analysis(line);
    } else if (strncmp(line, "FFT ", 4) == 0) {
        start_fft(line);
    } else if (strncmp(line, "SPECTRUM ", 9) == 0) {
        start_spectrum(line);
    } else if (strncmp(line, "ANALYSIS ", 9) == 0) {
        start_analysis_query(line);
    } else if (strncmp(line, "SWEEP ", 6) == 0) {
        start_sweep(line);
    } else if (strncmp(line, "FEATURE ", 8) == 0) {
        start_feature(line);
    } else if (strncmp(line, "ALERT ", 6) == 0) {
        start_alert(line);
    } else if (strncmp(line, "BATCH", 5) == 0) {
        start_batch(line);
    } else if (strncmp(line, "SCOPE", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        start_scope(line);
    } else if (strncmp(line, "GET ", 4) == 0) {
        start_get(line);
    } else if (strcmp(line, "CLEAR") == 0) {
        if (pd_acq_state() != PD_ACQ_IDLE) (void)reply("ERR CLEAR requires idle\r\n");
        else {
            pd_acq_clear_metadata();
            pd_analysis_clear();
            (void)reply("OK CLEAR acquisition_and_analysis_metadata\r\n");
        }
    } else if (line[0] != 0) {
        (void)reply("ERR unknown command; type HELP\r\n");
    }
}

static err_t receive_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    u16_t i;
    char c;
    (void)arg;
    (void)err;
    if (p == NULL) {
        tcp_recv(pcb, NULL);
        if (s_client == pcb) s_client = NULL;
        s_transfer.active = 0U;
        s_scope.enabled = 0U;
        (void)tcp_close(pcb);
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    for (i = 0U; i < p->tot_len; ++i) {
        (void)pbuf_copy_partial(p, &c, 1U, i);
        if (c == '\r' || c == '\n') {
            if (s_line_len != 0U) {
                s_line[s_line_len] = 0;
                execute_command(s_line);
                s_line_len = 0U;
            }
        } else if (c >= ' ' && c <= '~') {
            if (s_line_len + 1U < PD_TCP_LINE_BYTES) s_line[s_line_len++] = c;
            else { s_line_len = 0U; (void)reply("ERR command too long\r\n"); }
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void error_cb(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    s_client = NULL;
    s_line_len = 0U;
    s_transfer.active = 0U;
    s_scope.enabled = 0U;
}

static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || s_client != NULL) {
        (void)tcp_close(pcb);
        return ERR_ABRT;
    }
    s_client = pcb;
    s_line_len = 0U;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, receive_cb);
    tcp_err(pcb, error_cb);
    (void)reply("PD_ACQ TCP PS-2 READY; type HELP\r\n");
    return ERR_OK;
}

int pd_tcp_service_init(void)
{
    struct tcp_pcb *listener = tcp_new_ip_type(IPADDR_TYPE_ANY);
    err_t err;
    if (listener == NULL) return XST_FAILURE;
    err = tcp_bind(listener, IP_ANY_TYPE, PD_TCP_PORT);
    if (err != ERR_OK) return XST_FAILURE;
    listener = tcp_listen(listener);
    if (listener == NULL) return XST_FAILURE;
    tcp_accept(listener, accept_cb);
    return XST_SUCCESS;
}

void pd_tcp_service_poll(void)
{
    if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
    if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }
    xemacif_input(&echo_netif);
    transfer_pump();
}
