static void start_scope_frame(void)
{
    u32 ring_base, ring_size, wr_off, bytes, start, first, sum, sequence;
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
    sequence = s_scope.sequence++;
    (void)snprintf(out, sizeof(out),
        "SCOPE V1 seq=%lu samples=%lu bytes=%lu fs=%lu crc32=%08lx\r\n",
        (unsigned long)sequence, (unsigned long)s_scope.samples,
        (unsigned long)bytes, (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ,
        (unsigned long)sum);
    if (reply(out) != 0) return;
    s_scope.last_frame_sequence = sequence;
    s_scope.last_frame_valid = 1U;
    s_transfer.addr = (UINTPTR)s_scope_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    /* This buffer was just written by the CPU; invalidating it would discard
     * dirty cache lines before lwIP copies the payload. */
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/* Analyze the exact most recently transmitted live waveform frame on the PS.
 * The returned little-endian u16 array is a single-sided 1024-point Hann FFT:
 * bins 0..512, magnitude in approximate ADC peak-code units. */
static void start_scope_fft(char *text)
{
    u32 channel, bin, bytes = sizeof(s_scope_fft_buffer), sum;
    pd_spectrum_channel_t result;
    char out[256];
    char *cursor = text;

    /* Accept the explicit CHANNEL keyword used by the Qt live-scope client,
       while keeping the original compact `SCOPE FFT <channel>` form valid. */
    if (strncmp(cursor, "CHANNEL ", 8U) == 0) cursor += 8;
    if (parse_single_u32(cursor, &channel) != 0 || channel >= PD_SNAPSHOT_CHANNELS) {
        (void)reply("ERR usage: SCOPE FFT CHANNEL channel(0..3)\r\n");
        return;
    }
    if (!s_scope.enabled || !s_scope.last_frame_valid ||
        s_scope.samples != PD_FFT_POINTS) {
        (void)reply("ERR SCOPE FFT requires a completed live frame configured for exactly 1024 samples\r\n");
        return;
    }
    if (s_transfer.active) {
        (void)reply("ERR SCOPE transfer is still active\r\n");
        return;
    }
    if (pd_spectrum_analyze_channel(s_scope_buffer,
                                    s_scope.samples * PD_SCOPE_SAMPLE_BYTES,
                                    0U, PD_SPECTRUM_DEFAULT_FS_HZ, channel,
                                    s_scope_fft_bins, &result) != XST_SUCCESS) {
        (void)reply("ERR SCOPE FFT analysis failed\r\n");
        return;
    }
    /* The byte buffer is deliberately explicit little endian on the wire. */
    for (bin = 0U; bin <= PD_FFT_POINTS / 2U; ++bin) {
        const u16 magnitude = s_scope_fft_bins[bin];
        s_scope_fft_buffer[bin * 2U] = (u8)(magnitude & 0xFFU);
        s_scope_fft_buffer[bin * 2U + 1U] = (u8)(magnitude >> 8);
    }
    sum = crc32(s_scope_fft_buffer, bytes);
    (void)snprintf(out, sizeof(out),
        "SCOPE_FFT V1 seq=%lu channel=%lu samples=%u points=%u fs=%lu bin_hz=%lu peak_bin=%lu peak_hz=%lu amplitude=%lu dc=%lu bytes=%lu crc32=%08lx\r\n",
        (unsigned long)s_scope.last_frame_sequence, (unsigned long)channel,
        PD_FFT_POINTS, PD_FFT_POINTS,
        (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ,
        (unsigned long)(PD_SPECTRUM_DEFAULT_FS_HZ / PD_FFT_POINTS),
        (unsigned long)result.peak_bin, (unsigned long)result.peak_hz,
        (unsigned long)result.amplitude_code, (unsigned long)result.dc_code,
        (unsigned long)bytes, (unsigned long)sum);
    if (reply(out) != 0) return;
    s_transfer.addr = (UINTPTR)s_scope_fft_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/*
 * Return a waveform window centred on a retained PL peak event.
 *
 * The PL event packet carries phase but no sample timestamp or DDR byte
 * address. This selects the requested event word exactly, then maps its phase
 * into the newest archived 50 Hz snapshot as a best-effort candidate window.
 * The snapshot record has no cycle/sample ID proving that it contains that
 * event's transient; the header exposes both independent sequence numbers so
 * the host can avoid presenting this as guaranteed time correlation.
 */
static void start_scope_event(u32 event_sequence, u32 samples,
                              u32 has_event_selector, u32 requested_channel,
                              u32 requested_event_word_sequence)
{
    pd_event_record_t event_record;
    pd_snapshot_record_t snapshot_record;
    const u64 *event_words;
    u32 latest_snapshot;
    u32 phase = 0U, channel = 0U, phase_win, event_word_sequence = 0U;
    u32 i, total_samples, center, start, bytes, sum, scope_sequence;
    int have_peak = 0;
    char out[256];

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
    if (pd_acq_get_event_by_sequence(event_sequence, &event_record) != XST_SUCCESS ||
        event_record.bytes == 0U || event_record.bytes > PD_EVENT_ARCHIVE_STRIDE ||
        (event_record.bytes & 7U) != 0U) {
        (void)reply("ERR SCOPE event sequence is outside the retained archive window\r\n");
        return;
    }
    if (g_pd_acq.snapshot_sequence == 0U ||
        pd_acq_get_snapshot_by_sequence(g_pd_acq.snapshot_sequence - 1U,
                                        &snapshot_record) != XST_SUCCESS ||
        snapshot_record.archive_addr == 0U || snapshot_record.bytes < PD_SCOPE_SAMPLE_BYTES) {
        (void)reply("ERR SCOPE no archived snapshot is ready\r\n");
        return;
    }

    Xil_DCacheInvalidateRange((UINTPTR)event_record.ddr_addr, event_record.bytes);
    event_words = (const u64 *)(UINTPTR)event_record.ddr_addr;
    for (i = 0U; i < event_record.bytes / 8U; ++i) {
        const u64 word = event_words[i];
        /* Peak words have type 0x00; cycle summaries have type 0x01. */
        if ((u32)(word >> 56) == 0U) {
            const u32 word_channel = (u32)((word >> 25) & 0x3U);
            const u32 word_sequence = (u32)(word & 0x01FFFFFFU);
            if (has_event_selector != 0U &&
                (word_channel != requested_channel ||
                 word_sequence != requested_event_word_sequence))
                continue;
            phase = (u32)((word >> 28) & 0xFFFU);
            channel = word_channel;
            event_word_sequence = word_sequence;
            have_peak = 1;
            break;
        }
    }
    if (!have_peak) {
        (void)reply(has_event_selector != 0U ?
                    "ERR SCOPE event selector is not present in this record\r\n" :
                    "ERR SCOPE event has no peak word\r\n");
        return;
    }

    phase_win = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_CFG0(channel)) >> 16;
    if (phase_win == 0U) phase_win = 4096U;
    total_samples = snapshot_record.bytes / PD_SCOPE_SAMPLE_BYTES;
    if (total_samples < samples) {
        (void)reply("ERR SCOPE snapshot is shorter than requested window\r\n");
        return;
    }
    center = (phase * total_samples + (phase_win / 2U)) / phase_win;
    if (center >= total_samples) center = total_samples - 1U;
    start = center > samples / 2U ? center - samples / 2U : 0U;
    if (start + samples > total_samples) start = total_samples - samples;
    bytes = samples * PD_SCOPE_SAMPLE_BYTES;

    /* Snapshot data is contiguous and sample-aligned; pack it in the same
       little-endian 48-bit format used by SCOPE V1. */
    s_scope.last_frame_valid = 0U;
    Xil_DCacheInvalidateRange((UINTPTR)snapshot_record.archive_addr + start * PD_SCOPE_SAMPLE_BYTES,
                              bytes);
    for (i = 0U; i < samples; ++i) {
        u16 sample[PD_SNAPSHOT_CHANNELS];
        u8 *dst = &s_scope_buffer[i * PD_SCOPE_SAMPLE_BYTES];
        if (pd_snapshot_read_sample((const void *)(UINTPTR)snapshot_record.archive_addr,
                                    snapshot_record.bytes, start + i, sample) != XST_SUCCESS) {
            (void)reply("ERR SCOPE snapshot sample decode failed\r\n");
            return;
        }
        dst[0] = (u8)(sample[0] & 0xFFU);
        dst[1] = (u8)(((sample[0] >> 8) & 0x0FU) | ((sample[1] & 0x0FU) << 4));
        dst[2] = (u8)((sample[1] >> 4) & 0xFFU);
        dst[3] = (u8)(sample[2] & 0xFFU);
        dst[4] = (u8)(((sample[2] >> 8) & 0x0FU) | ((sample[3] & 0x0FU) << 4));
        dst[5] = (u8)((sample[3] >> 4) & 0xFFU);
    }
    sum = crc32(s_scope_buffer, bytes);
    scope_sequence = s_scope.sequence++;
    s_scope.last_frame_sequence = scope_sequence;
    s_scope.last_frame_valid = 1U;
    latest_snapshot = snapshot_record.sequence;
    (void)snprintf(out, sizeof(out),
        "SCOPE V2 seq=%lu samples=%lu bytes=%lu fs=%lu crc32=%08lx source=SNAP snap_seq=%lu event_seq=%lu event_channel=%lu event_word_seq=%lu start=%lu center=%lu phase=%lu\r\n",
        (unsigned long)scope_sequence, (unsigned long)samples,
        (unsigned long)bytes, (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ,
        (unsigned long)sum,
        (unsigned long)latest_snapshot, (unsigned long)event_sequence,
        (unsigned long)channel, (unsigned long)event_word_sequence,
        (unsigned long)start, (unsigned long)center, (unsigned long)phase);
    if (reply(out) != 0) return;
    s_transfer.addr = (UINTPTR)s_scope_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

/* Copy one retained PL event packet into a PS-owned transfer buffer.  The
 * peak phase is measured against sync_in by pd_feature_core, not inferred
 * from the unrelated raw DDR oscilloscope window.  This legacy single-record
 * response is retained for diagnostic clients; API 18 Qt uses the batch form. */
