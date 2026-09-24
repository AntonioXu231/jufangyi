/* PS-side PRPD aggregation over the retained PL feature-event archive. */
#ifndef PD_PRPD_H
#define PD_PRPD_H

#include "pd_acquisition.h"

#define PD_PRPD_PHASE_BINS 64U

typedef struct {
    u32 peak_count;
    u32 positive_count;
    u32 negative_count;
    u32 q_abs_mean;       /* Raw Q8.8 code average, not calibrated pC. */
    u32 q_abs_max;
    u32 phase_at_max;     /* Original PL phase index; default field width is 12 bits. */
    u32 phase_bins[PD_PRPD_PHASE_BINS];
} pd_prpd_channel_t;

typedef struct {
    u32 event_sequence_first;
    u32 event_sequence_next;
    u32 packet_count;
    pd_prpd_channel_t channel[4];
} pd_prpd_summary_t;

/* Reads a stable, retained event-sequence window and aggregates peak packets. */
int pd_prpd_build(u32 event_sequence_first, u32 event_sequence_next,
                  pd_prpd_summary_t *summary);

#endif /* PD_PRPD_H */
