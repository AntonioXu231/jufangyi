/* Decode the immutable 64-bit PL feature-event packet archive in PS DDR. */
#ifndef PD_EVENT_DECODE_H
#define PD_EVENT_DECODE_H

#include "pd_acquisition.h"

typedef struct {
    u32 event_sequence;
    u32 bytes;
    u32 peak_words;
    u32 cycle_words;
    u32 last_type;
    u32 peak_count[4];
    u32 q_abs_max[4];       /* Maximum |Q| in raw signed Q8.8 code units. */
    u32 cycle_count[4];
    u32 cycle_event_total[4];
    u32 cycle_qmax[4];
    u32 cycle_last_index[4];
} pd_event_summary_t;

/*
 * Decode one retained event packet.  The event packet format is the existing
 * PL contract: peak [63:56]=0, Q[55:40], phase[39:28], pol[27], ch[26:25];
 * cycle [63:56]=1, cycle-index[55:32], ch[31:30], n[29:16], qmax[15:0].
 */
int pd_event_summarize(const pd_event_record_t *record,
                       pd_event_summary_t *summary);

#endif /* PD_EVENT_DECODE_H */
