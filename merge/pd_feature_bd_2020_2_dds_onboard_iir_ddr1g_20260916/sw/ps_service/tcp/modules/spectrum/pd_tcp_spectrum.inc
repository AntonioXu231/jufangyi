/* One manual transfer carries a complete selected-channel SNAP plus its PS FFT. */
#define PD_SCOPE_FULL_SNAPSHOT_BYTES (PD_SCOPE_CYCLE_SAMPLES * PD_SCOPE_SAMPLE_BYTES)
#define PD_SCOPE_FULL_CHANNEL_BYTES  (PD_SCOPE_CYCLE_SAMPLES * 2U)
#define PD_SCOPE_FULL_FFT_BYTES      ((PD_FFT_POINTS / 2U + 1U) * 2U)
static u8 s_snapshot_full_wave_buffer[PD_SCOPE_FULL_CHANNEL_BYTES +
                                      PD_SCOPE_FULL_FFT_BYTES];

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

static void start_fft_snapshot_bins(const char *command)
{
    unsigned long sequence_arg = 0UL, channel_arg = 0UL, start_arg = 0UL;
    int consumed = 0;
    const char *tail;
    u32 bytes, i;
    pd_snapshot_record_t record;
    pd_spectrum_channel_t result;
    char header[256];

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR FFT BINS SNAP requires idle acquisition\r\n");
        return;
    }
    if (s_transfer.active || s_scope_envelope.active || s_scope_envelope.ready) {
        (void)reply("ERR FFT BINS transfer is busy\r\n");
        return;
    }
    if (sscanf(command, "SNAP SEQ %lu CHANNEL %lu START %lu%n",
               &sequence_arg, &channel_arg, &start_arg, &consumed) != 3 ||
        sequence_arg > 0xFFFFFFFFUL || channel_arg >= PD_SCOPE_ENV_CHANNELS ||
        start_arg > 0xFFFFFFFFUL) {
        (void)reply("ERR usage: FFT BINS SNAP SEQ seq CHANNEL channel(0..3) START sample\r\n");
        return;
    }
    tail = command + consumed;
    while (*tail != '\0' && isspace((unsigned char)*tail)) ++tail;
    if (*tail != '\0') {
        (void)reply("ERR usage: FFT BINS SNAP SEQ seq CHANNEL channel(0..3) START sample\r\n");
        return;
    }
    if (pd_acq_get_snapshot_by_sequence((u32)sequence_arg, &record) != XST_SUCCESS ||
        record.archive_addr == 0U) {
        (void)reply("ERR FFT BINS snapshot sequence is outside the retained archive window\r\n");
        return;
    }
    if ((record.bytes % PD_SNAPSHOT_BYTES_PER_SAMPLE) != 0U ||
        start_arg > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE ||
        PD_FFT_POINTS > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE - (u32)start_arg) {
        (void)reply("ERR FFT BINS requires 1024 complete samples inside SNAP\r\n");
        return;
    }

    Xil_DCacheInvalidateRange((UINTPTR)record.archive_addr +
                              (u32)start_arg * PD_SNAPSHOT_BYTES_PER_SAMPLE,
                              PD_FFT_POINTS * PD_SNAPSHOT_BYTES_PER_SAMPLE);
    if (pd_spectrum_analyze_channel((const void *)(UINTPTR)record.archive_addr,
                                    record.bytes, (u32)start_arg,
                                    s_fft_sample_rate_hz, (u32)channel_arg,
                                    s_scope_fft_bins, &result) != XST_SUCCESS) {
        (void)reply("ERR FFT BINS analysis failed\r\n");
        return;
    }

    for (i = 0U; i <= PD_FFT_POINTS / 2U; ++i) {
        s_scope_fft_buffer[i * 2U] = (u8)(s_scope_fft_bins[i] & 0xFFU);
        s_scope_fft_buffer[i * 2U + 1U] = (u8)(s_scope_fft_bins[i] >> 8);
    }
    bytes = (PD_FFT_POINTS / 2U + 1U) * 2U;
    (void)snprintf(header, sizeof(header),
        "FFT_SNAP_BINS_V1 seq=%lu index=%lu channel=%lu start=%lu snap_samples=%lu samples=%u points=%u fs=%lu bin_hz=%lu peak_bin=%lu peak_hz=%lu amplitude=%lu dc=%lu bytes=%lu crc32=%08lx\r\n",
        (unsigned long)record.sequence,
        (unsigned long)(record.sequence % PD_SNAP_ARCHIVE_COUNT),
        channel_arg, start_arg,
        (unsigned long)(record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE),
        PD_FFT_POINTS, PD_FFT_POINTS,
        (unsigned long)s_fft_sample_rate_hz,
        (unsigned long)(s_fft_sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)result.peak_bin, (unsigned long)result.peak_hz,
        (unsigned long)result.amplitude_code, (unsigned long)result.dc_code,
        (unsigned long)bytes, (unsigned long)crc32(s_scope_fft_buffer, bytes));
    if (reply(header) != 0) return;
    s_transfer.addr = (UINTPTR)s_scope_fft_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/* Return the exact 1024-point archived waveform window together with the PS
 * FFT bins, so the host can draw a genuinely matched time/frequency pair. */
static void start_fft_snapshot_wave(const char *command)
{
    unsigned long sequence_arg = 0UL, channel_arg = 0UL, start_arg = 0UL;
    int consumed = 0;
    const char *tail;
    u16 sample[PD_SNAPSHOT_CHANNELS];
    u32 i, bytes, sample_bytes = PD_FFT_POINTS * 2U;
    const u32 bins_bytes = (PD_FFT_POINTS / 2U + 1U) * 2U;
    pd_snapshot_record_t record;
    pd_spectrum_channel_t result;
    char header[320];

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR FFT WAVE SNAP requires idle acquisition\r\n");
        return;
    }
    if (s_transfer.active || s_scope_envelope.active || s_scope_envelope.ready) {
        (void)reply("ERR FFT WAVE SNAP transfer is busy\r\n");
        return;
    }
    if (sscanf(command, "%lu CHANNEL %lu START %lu%n",
               &sequence_arg, &channel_arg, &start_arg, &consumed) != 3 ||
        sequence_arg > 0xFFFFFFFFUL || channel_arg >= PD_SNAPSHOT_CHANNELS ||
        start_arg > 0xFFFFFFFFUL) {
        (void)reply("ERR usage: FFT WAVE SNAP SEQ seq CHANNEL channel(0..3) START sample\r\n");
        return;
    }
    tail = command + consumed;
    while (*tail != '\0' && isspace((unsigned char)*tail)) ++tail;
    if (*tail != '\0') {
        (void)reply("ERR usage: FFT WAVE SNAP SEQ seq CHANNEL channel(0..3) START sample\r\n");
        return;
    }
    if (pd_acq_get_snapshot_by_sequence((u32)sequence_arg, &record) != XST_SUCCESS ||
        record.archive_addr == 0U) {
        (void)reply("ERR FFT WAVE SNAP sequence is outside the retained archive window\r\n");
        return;
    }
    if ((record.bytes % PD_SNAPSHOT_BYTES_PER_SAMPLE) != 0U ||
        start_arg > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE ||
        PD_FFT_POINTS > record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE - (u32)start_arg) {
        (void)reply("ERR FFT WAVE SNAP requires 1024 complete samples inside SNAP\r\n");
        return;
    }

    Xil_DCacheInvalidateRange((UINTPTR)record.archive_addr +
                              (u32)start_arg * PD_SNAPSHOT_BYTES_PER_SAMPLE,
                              PD_FFT_POINTS * PD_SNAPSHOT_BYTES_PER_SAMPLE);
    for (i = 0U; i < PD_FFT_POINTS; ++i) {
        s32 centered;
        if (pd_snapshot_read_sample((const void *)(UINTPTR)record.archive_addr,
                                    record.bytes, (u32)start_arg + i,
                                    sample) != XST_SUCCESS) {
            (void)reply("ERR FFT WAVE SNAP sample decode failed\r\n");
            return;
        }
        centered = (s32)sample[channel_arg] - 2048;
        const u16 encoded = (u16)(s16)centered;
        s_snapshot_fft_wave_buffer[i * 2U] = (u8)(encoded & 0xFFU);
        s_snapshot_fft_wave_buffer[i * 2U + 1U] = (u8)(encoded >> 8);
    }
    if (pd_spectrum_analyze_channel((const void *)(UINTPTR)record.archive_addr,
                                    record.bytes, (u32)start_arg,
                                    s_fft_sample_rate_hz, (u32)channel_arg,
                                    s_scope_fft_bins, &result) != XST_SUCCESS) {
        (void)reply("ERR FFT WAVE SNAP analysis failed\r\n");
        return;
    }
    for (i = 0U; i <= PD_FFT_POINTS / 2U; ++i) {
        s_snapshot_fft_wave_buffer[sample_bytes + i * 2U] =
            (u8)(s_scope_fft_bins[i] & 0xFFU);
        s_snapshot_fft_wave_buffer[sample_bytes + i * 2U + 1U] =
            (u8)(s_scope_fft_bins[i] >> 8);
    }
    bytes = sample_bytes + bins_bytes;
    (void)snprintf(header, sizeof(header),
        "FFT_SNAP_WAVE_V1 seq=%lu index=%lu channel=%lu start=%lu snap_samples=%lu samples=%u points=%u fs=%lu bin_hz=%lu peak_bin=%lu peak_hz=%lu amplitude=%lu dc=%lu sample_bytes=%lu bins_bytes=%lu bytes=%lu crc32=%08lx\r\n",
        (unsigned long)record.sequence,
        (unsigned long)(record.sequence % PD_SNAP_ARCHIVE_COUNT),
        channel_arg, start_arg,
        (unsigned long)(record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE),
        PD_FFT_POINTS, PD_FFT_POINTS,
        (unsigned long)s_fft_sample_rate_hz,
        (unsigned long)(s_fft_sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)result.peak_bin, (unsigned long)result.peak_hz,
        (unsigned long)result.amplitude_code, (unsigned long)result.dc_code,
        (unsigned long)sample_bytes, (unsigned long)bins_bytes,
        (unsigned long)bytes,
        (unsigned long)crc32(s_snapshot_fft_wave_buffer, bytes));
    if (reply(header) != 0) return;
    s_transfer.addr = (UINTPTR)s_snapshot_fft_wave_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/* Return every selected-channel sample, then the PS-computed 1024-point bins. */
static void start_fft_snapshot_full(const char *command)
{
    unsigned long sequence_arg = 0UL, channel_arg = 0UL, start_arg = 0UL;
    int consumed = 0;
    const char *tail;
    const u8 *raw;
    u32 sample_count, i, bytes;
    pd_snapshot_record_t record;
    pd_spectrum_channel_t result;
    u16 sample[PD_SNAPSHOT_CHANNELS];
    char header[352];

    if (pd_acq_state() != PD_ACQ_IDLE) {
        (void)reply("ERR FFT FULL SNAP requires idle acquisition\r\n");
        return;
    }
    if (s_transfer.active || s_scope_envelope.active || s_scope_envelope.ready) {
        (void)reply("ERR FFT FULL SNAP transfer is busy\r\n");
        return;
    }
    if (sscanf(command, "%lu CHANNEL %lu START %lu%n",
               &sequence_arg, &channel_arg, &start_arg, &consumed) != 3 ||
        sequence_arg > 0xFFFFFFFFUL || channel_arg >= PD_SNAPSHOT_CHANNELS ||
        start_arg > 0xFFFFFFFFUL) {
        (void)reply("ERR usage: FFT FULL SNAP SEQ seq CHANNEL ch START sample\r\n");
        return;
    }
    tail = command + consumed;
    while (*tail != '\0' && isspace((unsigned char)*tail)) ++tail;
    if (*tail != '\0') {
        (void)reply("ERR usage: FFT FULL SNAP SEQ seq CHANNEL ch START sample\r\n");
        return;
    }
    if (pd_acq_get_snapshot_by_sequence((u32)sequence_arg, &record) != XST_SUCCESS ||
        record.archive_addr == 0U) {
        (void)reply("ERR FFT FULL SNAP sequence is outside the retained archive window\r\n");
        return;
    }
    if ((record.bytes % PD_SNAPSHOT_BYTES_PER_SAMPLE) != 0U) {
        (void)reply("ERR FFT FULL SNAP has an invalid sample stride\r\n");
        return;
    }
    sample_count = record.bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    if (record.bytes != PD_SCOPE_FULL_SNAPSHOT_BYTES ||
        sample_count != PD_SCOPE_CYCLE_SAMPLES ||
        start_arg > sample_count || PD_FFT_POINTS > sample_count - (u32)start_arg) {
        (void)reply("ERR FFT FULL SNAP requires a complete 50 Hz snapshot and valid FFT window\r\n");
        return;
    }

    Xil_DCacheInvalidateRange((UINTPTR)record.archive_addr, record.bytes);
    raw = (const u8 *)(UINTPTR)record.archive_addr;
    for (i = 0U; i < sample_count; ++i) {
        s32 centered;
        if (pd_snapshot_read_sample(raw, record.bytes, i, sample) != XST_SUCCESS) {
            (void)reply("ERR FFT FULL SNAP sample decode failed\r\n");
            return;
        }
        centered = (s32)sample[channel_arg] - 2048;
        {
            const u16 encoded = (u16)(s16)centered;
            s_snapshot_full_wave_buffer[i * 2U] = (u8)(encoded & 0xFFU);
            s_snapshot_full_wave_buffer[i * 2U + 1U] = (u8)(encoded >> 8);
        }
    }
    if (pd_spectrum_analyze_channel(raw, record.bytes, (u32)start_arg,
                                    s_fft_sample_rate_hz, (u32)channel_arg,
                                    s_scope_fft_bins, &result) != XST_SUCCESS) {
        (void)reply("ERR FFT FULL SNAP analysis failed\r\n");
        return;
    }
    for (i = 0U; i <= PD_FFT_POINTS / 2U; ++i) {
        s_snapshot_full_wave_buffer[PD_SCOPE_FULL_CHANNEL_BYTES + i * 2U] =
            (u8)(s_scope_fft_bins[i] & 0xFFU);
        s_snapshot_full_wave_buffer[PD_SCOPE_FULL_CHANNEL_BYTES + i * 2U + 1U] =
            (u8)(s_scope_fft_bins[i] >> 8);
    }

    bytes = PD_SCOPE_FULL_CHANNEL_BYTES + PD_SCOPE_FULL_FFT_BYTES;
    (void)snprintf(header, sizeof(header),
        "FFT_SNAP_FULL_V1 seq=%lu index=%lu channel=%lu start=%lu snap_samples=%lu points=%u fs=%lu bin_hz=%lu peak_bin=%lu peak_hz=%lu amplitude=%lu dc=%lu sample_bytes=%lu bins_bytes=%lu bytes=%lu crc32=%08lx\r\n",
        (unsigned long)record.sequence,
        (unsigned long)(record.sequence % PD_SNAP_ARCHIVE_COUNT),
        channel_arg, start_arg, (unsigned long)sample_count, PD_FFT_POINTS,
        (unsigned long)s_fft_sample_rate_hz,
        (unsigned long)(s_fft_sample_rate_hz / PD_FFT_POINTS),
        (unsigned long)result.peak_bin, (unsigned long)result.peak_hz,
        (unsigned long)result.amplitude_code, (unsigned long)result.dc_code,
        (unsigned long)PD_SCOPE_FULL_CHANNEL_BYTES,
        (unsigned long)PD_SCOPE_FULL_FFT_BYTES, (unsigned long)bytes,
        (unsigned long)crc32(s_snapshot_full_wave_buffer, bytes));
    if (reply(header) != 0) return;
    s_transfer.addr = (UINTPTR)s_snapshot_full_wave_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

static void start_fft(char *line)
{
    char *p = line + 4; /* "FFT " */
    u32 index, sequence, start_sample = 0U, auto_window = 0U;
    pd_snapshot_record_t record;
    pd_spectrum_result_t result;
    pd_spectrum_window_t window;

    if (strncmp(p, "WAVE SNAP SEQ ", 14U) == 0) {
        start_fft_snapshot_wave(p + 14U);
        return;
    }
    if (strncmp(p, "FULL SNAP SEQ ", 14U) == 0) {
        start_fft_snapshot_full(p + 14U);
        return;
    }

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
    if (strncmp(p, "BINS ", 5) == 0) {
        start_fft_snapshot_bins(p + 5);
        return;
    }
    if (strncmp(p, "AUTO ", 5) == 0) {
        auto_window = 1U;
        p += 5;
    }
    if (strncmp(p, "SNAP ", 5) != 0) {
        (void)reply("ERR usage: FFT FULL SNAP SEQ seq CHANNEL ch START sample | FFT WAVE SNAP SEQ seq CHANNEL ch START sample | FFT BINS SNAP SEQ seq CHANNEL ch START sample | FFT [AUTO] SNAP index|SEQ sequence [start_sample]\r\n");
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
