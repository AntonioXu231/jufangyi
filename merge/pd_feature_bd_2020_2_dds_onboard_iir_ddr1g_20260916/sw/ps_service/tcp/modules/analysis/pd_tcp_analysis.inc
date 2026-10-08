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
