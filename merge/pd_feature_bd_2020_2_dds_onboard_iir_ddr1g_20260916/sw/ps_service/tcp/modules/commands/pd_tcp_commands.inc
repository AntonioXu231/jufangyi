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
    if (s_transfer.active || s_scope_envelope.active || s_scope_envelope.ready)
        return; /* Never interleave text and raw payload/job response. */
    if (strcmp(line, "HELP") == 0) {
    (void)reply("CMD: START [n] | STOP | STATUS | CONFIG | SCOPE ON [samples]|NEXT|ENVELOPE|FFT CHANNEL n(0..3)|EVENT SEQ seq [samples [channel evt_seq]]|PEAKS [BATCH 1..32]|PHASE|OFF | REPORT | SET LIMIT n | SET FFTFS hz | SET ALERT DELTA n|MASK n | ALERT CONFIG|SWEEP n|SNAP SEQ n | RECOVER | CATALOG | EVENT n|SEQ n|DETAIL n|DETAIL SEQ n | PRPD SUMMARY|BINS ch first [count] | SNAP n|SEQ n | ANALYZE SNAP n|SEQ n | FFT CONFIG|SELFTEST|BINS SNAP SEQ n CHANNEL ch START sample|WAVE SNAP SEQ n CHANNEL ch START sample|FULL SNAP SEQ n CHANNEL ch START sample|SNAP n|SEQ n [start]|AUTO SNAP n|SEQ n | SPECTRUM [AUTO] SNAP n|SEQ n [start] | ANALYSIS n|SNAP SEQ n|CATALOG | SWEEP AUTO SNAP n|SEQ n|CATALOG|INDEX n|WINDOW n 0..4|SNAP SEQ n | FEATURE SWEEP n|SNAP SEQ n | BATCH AUTO | GET EVENT|SNAP n|SEQ n offset bytes | CLEAR\r\n");
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
        if (pd_acq_state() != PD_ACQ_IDLE || s_scope_envelope.active ||
            s_scope_envelope.ready || s_transfer.active)
            (void)reply("ERR CLEAR requires idle acquisition and no active transfer\r\n");
        else {
            pd_acq_clear_metadata();
            pd_analysis_clear();
            s_scope.peak_cursor = 0U;
            s_scope.peak_cursor_valid = 0U;
            s_scope_peak_skipped = 0U;
            s_scope_envelope.cached_valid = 0U;
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
        if (s_scope_envelope.active != 0U)
            (void)pd_acq_unpin_snapshot(s_scope_envelope.snapshot_sequence);
        s_scope_envelope.active = 0U;
        s_scope_envelope.ready = 0U;
        s_scope_envelope.cached_valid = 0U;
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
    if (s_scope_envelope.active != 0U)
        (void)pd_acq_unpin_snapshot(s_scope_envelope.snapshot_sequence);
    s_scope_envelope.active = 0U;
    s_scope_envelope.ready = 0U;
    s_scope_envelope.cached_valid = 0U;
}

static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || s_client != NULL) {
        (void)tcp_close(pcb);
        return ERR_ABRT;
    }
    s_client = pcb;
    s_scope.enabled = 0U;
    s_scope.peak_cursor_valid = 0U;
    s_scope_peak_skipped = 0U;
    s_scope_envelope.cached_valid = 0U;
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
    scope_envelope_step();
    transfer_pump();
}
