static void start_scope_peaks(void)
{
    u32 next, first, sequence, index, peaks, words_count, bytes, gap = 0U;
    u32 win[4], lock_mask = 0U, channel;
    pd_event_record_t record;
    const u64 *words;
    char out[256];

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
    if (s_scope.peak_cursor < first) {
        gap = first - s_scope.peak_cursor;
        s_scope_peak_skipped += gap;
        s_scope.peak_cursor = first;
    }
    if (s_scope.peak_cursor >= next) {
        (void)snprintf(out, sizeof(out),
            "PEAKS NONE first=%lu next=%lu skipped=%lu\r\n",
            (unsigned long)first, (unsigned long)next,
            (unsigned long)s_scope_peak_skipped);
        (void)reply(out);
        return;
    }
    sequence = s_scope.peak_cursor++;
    if (pd_acq_get_event_by_sequence(sequence, &record) != XST_SUCCESS ||
        record.bytes == 0U || record.bytes > PD_EVENT_ARCHIVE_STRIDE ||
        (record.bytes & 7U) != 0U) {
        ++s_scope_peak_skipped;
        (void)snprintf(out, sizeof(out), "PEAKS SKIP seq=%lu skipped=%lu\r\n",
            (unsigned long)sequence, (unsigned long)s_scope_peak_skipped);
        (void)reply(out);
        return;
    }
    Xil_DCacheInvalidateRange((UINTPTR)record.ddr_addr, record.bytes);
    words = (const u64 *)(UINTPTR)record.ddr_addr;
    words_count = record.bytes / 8U;
    if (words_count > (PD_EVENT_ARCHIVE_STRIDE / sizeof(u64))) {
        (void)reply("ERR SCOPE event packet exceeds transfer buffer\r\n");
        return;
    }
    peaks = 0U;
    for (index = 0U; index < words_count; ++index) {
        const u64 word = words[index];
        const u32 type = (u32)(word >> 56);
        if (type == 0U) ++peaks;
        else if (type != 1U) {
            ++s_scope_peak_skipped;
            (void)snprintf(out, sizeof(out), "PEAKS SKIP seq=%lu skipped=%lu\r\n",
                (unsigned long)sequence, (unsigned long)s_scope_peak_skipped);
            (void)reply(out);
            return;
        }
        s_scope_peak_buffer[index] = word;
    }
    bytes = record.bytes;
    for (channel = 0U; channel < 4U; ++channel) {
        win[channel] = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_CFG0(channel)) >> 16;
        if (Xil_In32(PD_FEATURE_BASE + PD_FEATURE_STATUS(channel)) & 1U)
            lock_mask |= 1U << channel;
    }
    (void)snprintf(out, sizeof(out),
        "PEAKS V2 seq=%lu total=%lu words=%lu bytes=%lu crc32=%08lx wins=%lu,%lu,%lu,%lu lock=%lx first=%lu next=%lu gap=%lu skipped=%lu\r\n",
        (unsigned long)sequence, (unsigned long)peaks,
        (unsigned long)words_count,
        (unsigned long)bytes,
        (unsigned long)crc32((const u8 *)s_scope_peak_buffer, bytes),
        (unsigned long)win[0], (unsigned long)win[1],
        (unsigned long)win[2], (unsigned long)win[3],
        (unsigned long)lock_mask, (unsigned long)first, (unsigned long)next,
        (unsigned long)gap, (unsigned long)s_scope_peak_skipped);
    if (reply(out) != 0) return;
    if (bytes == 0U) return;
    s_transfer.addr = (UINTPTR)s_scope_peak_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}
/* Return several retained records in one binary transfer.  The caller still
 * receives every event word; batching only reduces per-command TCP latency. */
static void start_scope_peaks_batch(u32 max_packets)
{
    u32 next, first, sequence, words_count, peaks, payload_bytes = 0U;
    u32 packets = 0U, total_peaks = 0U, gap = 0U, channel, lock_mask = 0U;
    u32 start_sequence = 0U, index;
    pd_event_record_t record;
    const u64 *words;
    const u8 *raw;
    char out[256];

    if (!s_scope.enabled) {
        (void)reply("ERR SCOPE is off; send SCOPE ON [samples] first\r\n");
        return;
    }
    if (s_transfer.active) {
        (void)reply("ERR SCOPE transfer is still active\r\n");
        return;
    }
    if (max_packets == 0U || max_packets > PD_SCOPE_PEAK_BATCH_MAX_PACKETS) {
        (void)reply("ERR SCOPE PEAKS BATCH count must be 1..32\r\n");
        return;
    }

    next = g_pd_acq.event_sequence;
    first = next > PD_EVENT_ARCHIVE_COUNT ? next - PD_EVENT_ARCHIVE_COUNT : 0U;
    if (s_scope.peak_cursor < first) {
        gap = first - s_scope.peak_cursor;
        s_scope_peak_skipped += gap;
        s_scope.peak_cursor = first;
    }
    start_sequence = s_scope.peak_cursor;

    while (s_scope.peak_cursor < next && packets < max_packets) {
        sequence = s_scope.peak_cursor;
        if (pd_acq_get_event_by_sequence(sequence, &record) != XST_SUCCESS ||
            record.bytes == 0U || record.bytes > PD_EVENT_ARCHIVE_STRIDE ||
            record.ddr_addr == 0U ||
            (record.bytes & 7U) != 0U) {
            ++s_scope.peak_cursor;
            ++s_scope_peak_skipped;
            continue;
        }
        if (payload_bytes > PD_SCOPE_PEAK_BATCH_BYTES - 8U - record.bytes)
            break;

        Xil_DCacheInvalidateRange((UINTPTR)record.ddr_addr, record.bytes);
        raw = (const u8 *)(UINTPTR)record.ddr_addr;
        words = (const u64 *)raw;
        words_count = record.bytes / 8U;
        peaks = 0U;
        {
            u32 malformed = 0U;
            for (index = 0U; index < words_count; ++index) {
                const u32 type = (u32)(words[index] >> 56);
                if (type == 0U) ++peaks;
                else if (type != 1U) { malformed = 1U; break; }
            }
            /* Reject malformed records atomically; do not put a partial packet
             * in the batch or repeatedly retry the same bad archive slot. */
            if (malformed != 0U) {
                ++s_scope.peak_cursor;
                ++s_scope_peak_skipped;
                continue;
            }
        }

        store_u32_le(&s_scope_peak_batch_buffer[payload_bytes], sequence);
        store_u32_le(&s_scope_peak_batch_buffer[payload_bytes + 4U], record.bytes);
        memcpy(&s_scope_peak_batch_buffer[payload_bytes + 8U], raw, record.bytes);
        payload_bytes += 8U + record.bytes;
        total_peaks += peaks;
        ++packets;
        ++s_scope.peak_cursor;
    }

    if (packets == 0U) {
        (void)snprintf(out, sizeof(out),
            "PEAKS NONE first=%lu next=%lu skipped=%lu\r\n",
            (unsigned long)first, (unsigned long)next,
            (unsigned long)s_scope_peak_skipped);
        (void)reply(out);
        return;
    }

    {
        u32 win[4];
        for (channel = 0U; channel < 4U; ++channel) {
            win[channel] = Xil_In32(PD_FEATURE_BASE + PD_FEATURE_CFG0(channel)) >> 16;
            if (Xil_In32(PD_FEATURE_BASE + PD_FEATURE_STATUS(channel)) & 1U)
                lock_mask |= 1U << channel;
        }
        (void)snprintf(out, sizeof(out),
            "PEAKS V3 packets=%lu peaks=%lu bytes=%lu crc32=%08lx wins=%lu,%lu,%lu,%lu lock=%lx first=%lu next=%lu start=%lu gap=%lu skipped=%lu\r\n",
            (unsigned long)packets, (unsigned long)total_peaks,
            (unsigned long)payload_bytes,
            (unsigned long)crc32(s_scope_peak_batch_buffer, payload_bytes),
            (unsigned long)win[0], (unsigned long)win[1],
            (unsigned long)win[2], (unsigned long)win[3],
            (unsigned long)lock_mask, (unsigned long)first,
            (unsigned long)next, (unsigned long)start_sequence,
            (unsigned long)gap, (unsigned long)s_scope_peak_skipped);
    }
    if (reply(out) != 0) return;
    s_transfer.addr = (UINTPTR)s_scope_peak_batch_buffer;
    s_transfer.offset = 0U;
    s_transfer.remaining = payload_bytes;
    s_transfer.invalidate_cache = 0U;
    s_transfer.active = 1U;
}

