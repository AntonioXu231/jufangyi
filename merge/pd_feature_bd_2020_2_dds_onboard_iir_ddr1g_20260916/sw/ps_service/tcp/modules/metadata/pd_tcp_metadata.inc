static void reply_status(void)
{
    char out[320];
    (void)snprintf(out, sizeof(out),
        "STATUS state=%lu packets=%lu events=%lu snaps=%lu analysis=%lu sweeps=%lu ev_ovw=%lu snap_ovw=%lu snap_pin_drop=%lu recov=%lu disc=%lu ddr=%08lx slot=%08lx dma=%08lx drops=%lu err=%s\r\n",
        (unsigned long)pd_acq_state(), (unsigned long)pd_acq_packets(),
        (unsigned long)g_pd_acq.event_sequence, (unsigned long)g_pd_acq.snapshot_sequence,
        (unsigned long)pd_analysis_next_sequence(),
        (unsigned long)pd_analysis_sweep_next_sequence(),
        (unsigned long)g_pd_acq.event_overwrites, (unsigned long)g_pd_acq.snapshot_overwrites,
        (unsigned long)g_pd_acq.snapshot_pin_drops,
        (unsigned long)g_pd_acq.recovery_count,
        (unsigned long)g_pd_acq.recovery_discarded_slots,
        (unsigned long)ddr_read(PD_DDR_STATUS), (unsigned long)ddr_read(PD_SLOT_STATUS),
        (unsigned long)dma_read(PD_S2MM_DMASR_OFFSET),
        (unsigned long)ddr_read(PD_SLOT_DROPS), pd_acq_last_error());
    (void)reply(out);
}

static void reply_config(void)
{
    u32 scale[4];
    u32 channel;
    char out[512];

    for (channel = 0U; channel < 4U; ++channel)
        scale[channel] = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_SCALE(channel)) & 0xFFFFU;
    (void)snprintf(out, sizeof(out),
        "CONFIG api=18 sample_hz=%lu default_limit=%lu event_slots=%u event_stride=%lu snap_slots=%u snap_stride=%lu analysis_slots=%u sweep_slots=%u sweep_windows=%u prpd_bins=%u scope=%lu/%lu scope_fft=ps_q15_1024x1_bins513 snap_fft=ps_q15_1024x1x1_bins513 snap_fft_wave=ps_q15_1024x1_samples_plus_bins snap_full=raw6b_1300000 event_meta=per_packet_v1 scope_env=cycle1300000_bins1024_ch4_minmax16 scope_peaks_batch=32 scale_q88=%lu,%lu,%lu,%lu alert_mask=%lx alert_delta=%ld get_max=%u state=%lu\r\n",
        (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ,
        (unsigned long)pd_acq_default_packet_limit(), PD_EVENT_ARCHIVE_COUNT,
        (unsigned long)PD_EVENT_ARCHIVE_STRIDE, PD_SNAP_ARCHIVE_COUNT,
        (unsigned long)PD_SNAP_ARCHIVE_STRIDE, PD_ANALYSIS_ARCHIVE_COUNT,
        PD_ANALYSIS_ARCHIVE_COUNT, PD_ANALYSIS_SWEEP_WINDOWS, PD_PRPD_PHASE_BINS,
        (unsigned long)s_scope.enabled, (unsigned long)s_scope.samples,
        (unsigned long)scale[0], (unsigned long)scale[1],
        (unsigned long)scale[2], (unsigned long)scale[3],
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
        "REPORT api=18 state=%lu event_seq=[%lu,%lu) snap_seq=[%lu,%lu) analysis_seq=[%lu,%lu) sweep_seq=[%lu,%lu) alert_mask=0x%lx alert_delta=%ld ddr=%08lx slot=%08lx dma=%08lx drops=%lu err=%s\r\n",
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
