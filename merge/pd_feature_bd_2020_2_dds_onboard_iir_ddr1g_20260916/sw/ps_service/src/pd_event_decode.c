#include "pd_event_decode.h"

#include "xil_cache.h"
#include "xstatus.h"
#include <string.h>

#define PD_EVENT_TYPE_PEAK  0U
#define PD_EVENT_TYPE_CYCLE 1U

int pd_event_summarize(const pd_event_record_t *record,
                       pd_event_summary_t *summary)
{
    const volatile u64 *words;
    u32 index, count;

    if (record == 0 || summary == 0 || record->bytes == 0U ||
        (record->bytes & 7U) != 0U)
        return XST_FAILURE;

    Xil_DCacheInvalidateRange((UINTPTR)record->ddr_addr, record->bytes);
    (void)memset(summary, 0, sizeof(*summary));
    summary->event_sequence = record->sequence;
    summary->bytes = record->bytes;
    words = (const volatile u64 *)(UINTPTR)record->ddr_addr;
    count = record->bytes / 8U;

    for (index = 0U; index < count; ++index) {
        u64 word = words[index];
        u32 type = (u32)(word >> 56);
        if (type == PD_EVENT_TYPE_PEAK) {
            s32 q = (s32)(s16)((word >> 40) & 0xFFFFU);
            u32 absolute_q = q < 0 ? (u32)(-q) : (u32)q;
            u32 channel = (u32)((word >> 25) & 0x3U);
            ++summary->peak_words;
            ++summary->peak_count[channel];
            if (absolute_q > summary->q_abs_max[channel])
                summary->q_abs_max[channel] = absolute_q;
        } else if (type == PD_EVENT_TYPE_CYCLE) {
            u32 channel = (u32)((word >> 30) & 0x3U);
            ++summary->cycle_words;
            ++summary->cycle_count[channel];
            summary->cycle_event_total[channel] += (u32)((word >> 16) & 0x3FFFU);
            summary->cycle_qmax[channel] = (u32)(word & 0xFFFFU);
            summary->cycle_last_index[channel] = (u32)((word >> 32) & 0x00FFFFFFU);
        } else {
            return XST_FAILURE;
        }
        summary->last_type = type;
    }
    if (summary->last_type != PD_EVENT_TYPE_CYCLE ||
        summary->peak_words != record->peak_words ||
        summary->cycle_words != record->cycle_words)
        return XST_FAILURE;
    return XST_SUCCESS;
}
