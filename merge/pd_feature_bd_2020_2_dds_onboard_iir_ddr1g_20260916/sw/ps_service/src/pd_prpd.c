#include "pd_prpd.h"

#include "xil_cache.h"
#include "xstatus.h"
#include <string.h>

#define PD_EVENT_TYPE_PEAK 0U

int pd_prpd_build(u32 event_sequence_first, u32 event_sequence_next,
                  pd_prpd_summary_t *summary)
{
    u32 sequence, channel;
    u64 q_sum[4] = { 0U, 0U, 0U, 0U };

    if (summary == 0 || event_sequence_first >= event_sequence_next ||
        event_sequence_next > g_pd_acq.event_sequence ||
        event_sequence_next - event_sequence_first > PD_EVENT_ARCHIVE_COUNT)
        return XST_FAILURE;

    (void)memset(summary, 0, sizeof(*summary));
    summary->event_sequence_first = event_sequence_first;
    summary->event_sequence_next = event_sequence_next;

    for (sequence = event_sequence_first; sequence < event_sequence_next; ++sequence) {
        pd_event_record_t record;
        const volatile u64 *words;
        u32 word_index, word_count;

        if (pd_acq_get_event_by_sequence(sequence, &record) != XST_SUCCESS)
            return XST_FAILURE;
        Xil_DCacheInvalidateRange((UINTPTR)record.ddr_addr, record.bytes);
        words = (const volatile u64 *)(UINTPTR)record.ddr_addr;
        word_count = record.bytes / 8U;
        ++summary->packet_count;

        for (word_index = 0U; word_index < word_count; ++word_index) {
            u64 word = words[word_index];
            if ((u32)(word >> 56) == PD_EVENT_TYPE_PEAK) {
                s32 q = (s32)(s16)((word >> 40) & 0xFFFFU);
                u32 absolute_q = q < 0 ? (u32)(-q) : (u32)q;
                u32 phase = (u32)((word >> 28) & 0xFFFU);
                u32 polarity = (u32)((word >> 27) & 0x1U);
                u32 bin;
                channel = (u32)((word >> 25) & 0x3U);
                bin = phase >> 6; /* Default 12-bit phase field into 64 display bins. */
                if (bin >= PD_PRPD_PHASE_BINS) bin = PD_PRPD_PHASE_BINS - 1U;
                ++summary->channel[channel].peak_count;
                ++summary->channel[channel].phase_bins[bin];
                if (polarity != 0U) ++summary->channel[channel].positive_count;
                else ++summary->channel[channel].negative_count;
                q_sum[channel] += absolute_q;
                if (absolute_q > summary->channel[channel].q_abs_max) {
                    summary->channel[channel].q_abs_max = absolute_q;
                    summary->channel[channel].phase_at_max = phase;
                }
            }
        }
    }
    for (channel = 0U; channel < 4U; ++channel) {
        if (summary->channel[channel].peak_count != 0U)
            summary->channel[channel].q_abs_mean =
                (u32)(q_sum[channel] / summary->channel[channel].peak_count);
    }
    return XST_SUCCESS;
}
