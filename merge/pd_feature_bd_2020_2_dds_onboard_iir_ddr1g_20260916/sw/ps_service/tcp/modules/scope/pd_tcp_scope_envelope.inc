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

static void prepare_scope_envelope_header(void)
{
    (void)snprintf(s_scope_envelope.header, sizeof(s_scope_envelope.header),
        "SCOPE_ENV V1 seq=%lu snap_seq=%lu samples=%lu bins=%u bytes=%u fs=%lu lock=%lx phase=cycle_start crc32=%08lx\r\n",
        (unsigned long)s_scope.sequence++,
        (unsigned long)s_scope_envelope.snapshot_sequence,
        (unsigned long)s_scope_envelope.sample_count,
        PD_SCOPE_ENV_BINS, PD_SCOPE_ENV_BYTES,
        (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ,
        (unsigned long)s_scope_envelope.lock_mask,
        (unsigned long)s_scope_envelope.checksum);
}

/* Start an asynchronous full-cycle envelope job.  The heavy 1.3M-sample
 * reduction is deliberately performed by pd_tcp_service_poll(), in bounded
 * chunks, so the lwIP receive callback never monopolizes the acquisition loop. */
static void start_scope_envelope(void)
{
    pd_snapshot_record_t snapshot;
    u32 next;
    u32 channel;

    if (!s_scope.enabled) {
        (void)reply("ERR SCOPE is off; send SCOPE ON [samples] first\r\n");
        return;
    }
    if (pd_acq_state() != PD_ACQ_RUNNING) {
        (void)reply("ERR SCOPE ENVELOPE requires a running acquisition\r\n");
        return;
    }
    if (s_transfer.active || s_scope_envelope.active || s_scope_envelope.ready) {
        (void)reply("ERR SCOPE transfer is still active\r\n");
        return;
    }
    next = g_pd_acq.snapshot_sequence;
    if (next == 0U ||
        pd_acq_get_snapshot_by_sequence(next - 1U, &snapshot) != XST_SUCCESS ||
        snapshot.archive_addr == 0U ||
        snapshot.bytes != PD_SCOPE_CYCLE_SAMPLES * PD_SCOPE_SAMPLE_BYTES) {
        (void)reply("SCOPE_ENV NONE reason=not_ready\r\n");
        return;
    }

    /* The UI polls more often than the archive produces a new full-cycle
     * snapshot. Reuse the completed envelope until the SNAP sequence changes;
     * this preserves the V1 payload and cadence without rescanning 3.12 MiB. */
    if (s_scope_envelope.cached_valid != 0U &&
        s_scope_envelope.snapshot_sequence == snapshot.sequence &&
        s_scope_envelope.snapshot_addr == snapshot.archive_addr &&
        s_scope_envelope.snapshot_bytes == snapshot.bytes) {
        prepare_scope_envelope_header();
        s_scope_envelope.ready = 1U;
        return;
    }

    /* Keep the four-slot PS archive record immutable for the full reduction. */
    if (pd_acq_pin_snapshot(snapshot.sequence) != XST_SUCCESS) {
        (void)reply("SCOPE_ENV NONE reason=snapshot_busy\r\n");
        return;
    }

    memset(&s_scope_envelope, 0, sizeof(s_scope_envelope));
    s_scope_envelope.snapshot_sequence = snapshot.sequence;
    s_scope_envelope.snapshot_bytes = snapshot.bytes;
    s_scope_envelope.snapshot_addr = snapshot.archive_addr;
    s_scope_envelope.sample_count = snapshot.bytes / PD_SCOPE_SAMPLE_BYTES;
    for (channel = 0U; channel < PD_SCOPE_ENV_CHANNELS; ++channel) {
        if (Xil_In32(PD_FEATURE_BASE + PD_FEATURE_STATUS(channel)) & 1U)
            s_scope_envelope.lock_mask |= 1U << channel;
    }
    Xil_DCacheInvalidateRange((UINTPTR)snapshot.archive_addr, snapshot.bytes);
    s_scope_envelope.active = 1U;
}

static void finish_scope_envelope_bin(u32 bin)
{
    u32 channel;
    u32 offset = bin * PD_SCOPE_ENV_CHANNELS * 4U;
    for (channel = 0U; channel < PD_SCOPE_ENV_CHANNELS; ++channel) {
        const u16 lo = (u16)s_scope_envelope.minimum[bin][channel];
        const u16 hi = (u16)s_scope_envelope.maximum[bin][channel];
        s_scope_envelope.payload[offset++] = (u8)(lo & 0xFFU);
        s_scope_envelope.payload[offset++] = (u8)(lo >> 8);
        s_scope_envelope.payload[offset++] = (u8)(hi & 0xFFU);
        s_scope_envelope.payload[offset++] = (u8)(hi >> 8);
    }
}

static void scope_envelope_step(void)
{
    pd_snapshot_record_t current;
    const u8 *raw;
    u32 budget = PD_SCOPE_ENV_STEP_SAMPLES;

    if (s_scope_envelope.active) {
        if (s_client == NULL ||
            pd_acq_get_snapshot_by_sequence(s_scope_envelope.snapshot_sequence,
                                             &current) != XST_SUCCESS ||
            current.archive_addr != s_scope_envelope.snapshot_addr ||
            current.bytes != s_scope_envelope.snapshot_bytes) {
            s_scope_envelope.active = 0U;
            s_scope_envelope.cached_valid = 0U;
            (void)pd_acq_unpin_snapshot(s_scope_envelope.snapshot_sequence);
            if (s_client != NULL)
                (void)reply("SCOPE_ENV SKIP reason=snapshot_overwritten\r\n");
            return;
        }
        raw = (const u8 *)(UINTPTR)s_scope_envelope.snapshot_addr;
        while (budget != 0U && s_scope_envelope.bin_cursor < PD_SCOPE_ENV_BINS) {
            const u32 bin = s_scope_envelope.bin_cursor;
            const u32 bin_start = (bin * s_scope_envelope.sample_count) /
                                  PD_SCOPE_ENV_BINS;
            const u32 bin_end = ((bin + 1U) * s_scope_envelope.sample_count) /
                                PD_SCOPE_ENV_BINS;
            if (s_scope_envelope.sample_cursor == bin_start) {
                u32 channel;
                for (channel = 0U; channel < PD_SCOPE_ENV_CHANNELS; ++channel) {
                    s_scope_envelope.minimum[bin][channel] = SHRT_MAX;
                    s_scope_envelope.maximum[bin][channel] = SHRT_MIN;
                }
            }
            while (budget != 0U && s_scope_envelope.sample_cursor < bin_end) {
                const u8 *p = raw + s_scope_envelope.sample_cursor * PD_SCOPE_SAMPLE_BYTES;
                const s16 sample0 =
                    (s16)((u16)p[0] | ((u16)(p[1] & 0x0FU) << 8)) - 2048;
                const s16 sample1 =
                    (s16)(((u16)p[1] >> 4) | ((u16)p[2] << 4)) - 2048;
                const s16 sample2 =
                    (s16)((u16)p[3] | ((u16)(p[4] & 0x0FU) << 8)) - 2048;
                const s16 sample3 =
                    (s16)(((u16)p[4] >> 4) | ((u16)p[5] << 4)) - 2048;
                if (sample0 < s_scope_envelope.minimum[bin][0])
                    s_scope_envelope.minimum[bin][0] = sample0;
                if (sample0 > s_scope_envelope.maximum[bin][0])
                    s_scope_envelope.maximum[bin][0] = sample0;
                if (sample1 < s_scope_envelope.minimum[bin][1])
                    s_scope_envelope.minimum[bin][1] = sample1;
                if (sample1 > s_scope_envelope.maximum[bin][1])
                    s_scope_envelope.maximum[bin][1] = sample1;
                if (sample2 < s_scope_envelope.minimum[bin][2])
                    s_scope_envelope.minimum[bin][2] = sample2;
                if (sample2 > s_scope_envelope.maximum[bin][2])
                    s_scope_envelope.maximum[bin][2] = sample2;
                if (sample3 < s_scope_envelope.minimum[bin][3])
                    s_scope_envelope.minimum[bin][3] = sample3;
                if (sample3 > s_scope_envelope.maximum[bin][3])
                    s_scope_envelope.maximum[bin][3] = sample3;
                ++s_scope_envelope.sample_cursor;
                --budget;
            }
            if (s_scope_envelope.sample_cursor == bin_end) {
                finish_scope_envelope_bin(bin);
                ++s_scope_envelope.bin_cursor;
            }
        }
        if (s_scope_envelope.bin_cursor == PD_SCOPE_ENV_BINS) {
            s_scope_envelope.checksum = crc32(s_scope_envelope.payload,
                                              PD_SCOPE_ENV_BYTES);
            s_scope_envelope.cached_valid = 1U;
            prepare_scope_envelope_header();
            s_scope_envelope.active = 0U;
            s_scope_envelope.ready = 1U;
            (void)pd_acq_unpin_snapshot(s_scope_envelope.snapshot_sequence);
        }
    }
    if (s_scope_envelope.ready && s_client != NULL && !s_transfer.active &&
        reply(s_scope_envelope.header) == 0) {
        s_transfer.addr = (UINTPTR)s_scope_envelope.payload;
        s_transfer.offset = 0U;
        s_transfer.remaining = PD_SCOPE_ENV_BYTES;
        s_transfer.invalidate_cache = 0U;
        s_transfer.active = 1U;
        s_scope_envelope.ready = 0U;
    }
}

static void start_scope(char *line)
{
    char *p = line + 5;
    u32 samples;
    while (*p == ' ') ++p;
    if (strncmp(p, "OFF", 3) == 0 && p[3] == 0) {
        s_scope.enabled = 0U;
        s_scope.last_frame_valid = 0U;
        if (s_scope_envelope.active != 0U) {
            (void)pd_acq_unpin_snapshot(s_scope_envelope.snapshot_sequence);
            s_scope_envelope.active = 0U;
            s_scope_envelope.cached_valid = 0U;
        }
        s_scope_envelope.ready = 0U;
        (void)reply("OK SCOPE OFF\r\n");
        return;
    }
    if (strncmp(p, "NEXT", 4) == 0 && p[4] == 0) {
        start_scope_frame();
        return;
    }
    if (strncmp(p, "ENVELOPE", 8) == 0 && p[8] == 0) {
        start_scope_envelope();
        return;
    }
    if (strncmp(p, "FFT ", 4) == 0) {
        start_scope_fft(p + 4);
        return;
    }
    if (strncmp(p, "EVENT SEQ ", 10) == 0) {
        char *cursor = p + 10;
        u32 event_sequence, event_samples, event_channel, event_word_sequence;
        u32 has_event_selector;
        if (parse_scope_event_args(cursor, &event_sequence, &event_samples,
                                   &event_channel, &event_word_sequence,
                                   &has_event_selector) != 0) {
            (void)reply("ERR usage: SCOPE EVENT SEQ sequence [samples [channel event_word_seq]]\r\n");
            return;
        }
        if (event_samples == 0U) event_samples = s_scope.samples;
        if (event_samples < 256U || event_samples > PD_SCOPE_MAX_SAMPLES ||
            (event_samples % 4U) != 0U) {
            (void)reply("ERR SCOPE samples must be 256..2048 and divisible by 4\r\n");
            return;
        }
        start_scope_event(event_sequence, event_samples, has_event_selector,
                          event_channel, event_word_sequence);
        return;
    }
    if (strncmp(p, "PEAKS BATCH ", 12U) == 0) {
        u32 batch_packets;
        if (parse_single_u32(p + 12U, &batch_packets) != 0) {
            (void)reply("ERR usage: SCOPE PEAKS BATCH 1..32\r\n");
            return;
        }
        start_scope_peaks_batch(batch_packets);
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
        (void)reply("ERR usage: SCOPE ON [samples] | NEXT | ENVELOPE | FFT CHANNEL channel(0..3) | EVENT SEQ seq [samples [channel evt_seq]] | PEAKS [BATCH 1..32] | PHASE | OFF\r\n");
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
    s_scope.last_frame_valid = 0U;
    if (!s_scope.peak_cursor_valid) {
        const u32 next = g_pd_acq.event_sequence;
        s_scope.peak_cursor = next > PD_EVENT_ARCHIVE_COUNT ?
                              next - PD_EVENT_ARCHIVE_COUNT : 0U;
        s_scope_peak_skipped = 0U;
        s_scope.peak_cursor_valid = 1U;
    }
    {
        char out[96];
        (void)snprintf(out, sizeof(out),
            "OK SCOPE ON samples=%lu fs=%lu; send SCOPE NEXT\r\n",
            (unsigned long)s_scope.samples,
            (unsigned long)PD_SPECTRUM_DEFAULT_FS_HZ);
        (void)reply(out);
    }
}
