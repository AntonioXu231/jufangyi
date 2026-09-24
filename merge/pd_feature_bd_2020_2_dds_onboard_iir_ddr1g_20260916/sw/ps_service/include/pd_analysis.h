/* Persistent, PS-side spectrum-analysis archive.  No PL register contract is used here. */
#ifndef PD_ANALYSIS_H
#define PD_ANALYSIS_H

#include "pd_acquisition.h"
#include "pd_spectrum.h"

#define PD_ANALYSIS_ARCHIVE_COUNT 4U
#define PD_ANALYSIS_SWEEP_WINDOWS 5U

typedef struct {
    u32 magic;
    u32 version;
    u32 analysis_sequence; /* Monotonic PS analysis record number. */
    u32 snapshot_sequence; /* Immutable sequence of the archived input snapshot. */
    u32 snapshot_index;    /* Archive-ring index used when the analysis ran. */
    u32 snapshot_bytes;
    u32 window_auto;       /* 1: strongest-excursion window; 0: caller-selected start. */
    pd_spectrum_window_t window;
    pd_spectrum_result_t spectrum;
} pd_analysis_record_t;

/* Run a fixed or automatically selected FFT window and save its result in a 4-record ring. */
int pd_analysis_run(const pd_snapshot_record_t *snapshot, u32 snapshot_index,
                    u32 auto_window, u32 start_sample, u32 sample_rate_hz,
                    pd_analysis_record_t *result);

/* Index and snapshot-sequence lookups reject overwritten analysis records. */
int pd_analysis_get(u32 index, pd_analysis_record_t *result);
int pd_analysis_get_by_snapshot_sequence(u32 snapshot_sequence,
                                         pd_analysis_record_t *result);
u32 pd_analysis_next_sequence(void);
void pd_analysis_clear(void);

/* Five adjacent 1024-point windows centered on one automatically found event. */
typedef struct {
    u32 magic;
    u32 version;
    u32 sweep_sequence;
    u32 snapshot_sequence;
    u32 snapshot_index;
    u32 snapshot_bytes;
    pd_spectrum_window_t event;
    u32 start_sample[PD_ANALYSIS_SWEEP_WINDOWS];
    pd_spectrum_result_t spectrum[PD_ANALYSIS_SWEEP_WINDOWS];
} pd_analysis_sweep_t;

/* Relative non-DC spectral-power trend across sweep windows 0..4. */
typedef struct {
    u64 pre_band_power;       /* Mean of windows 0 and 1. */
    u64 event_band_power;     /* Window 2, centred on the detected event. */
    u64 post_band_power;      /* Mean of windows 3 and 4. */
    s32 event_delta_permille; /* (event - pre) / pre * 1000. */
    s32 recovery_permille;    /* (post - pre) / pre * 1000. */
} pd_sweep_feature_channel_t;

typedef struct {
    u32 sweep_sequence;
    u32 snapshot_sequence;
    u32 snapshot_index;
    pd_spectrum_window_t event;
    pd_sweep_feature_channel_t channel[PD_SNAPSHOT_CHANNELS];
} pd_sweep_feature_t;

int pd_analysis_sweep_run(const pd_snapshot_record_t *snapshot, u32 snapshot_index,
                          u32 sample_rate_hz, pd_analysis_sweep_t *result);
int pd_analysis_sweep_get(u32 index, pd_analysis_sweep_t *result);
int pd_analysis_sweep_get_by_snapshot_sequence(u32 snapshot_sequence,
                                               pd_analysis_sweep_t *result);
u32 pd_analysis_sweep_next_sequence(void);
int pd_analysis_sweep_feature(const pd_analysis_sweep_t *sweep,
                              pd_sweep_feature_t *feature);

/* Rule-only verdict: no hardware alarm, interrupt, or persistent side effect. */
typedef struct {
    u32 selected_mask;          /* Enabled channels, bits 0..3. */
    s32 delta_threshold_permille;
    u32 hit_mask;               /* Selected channels whose event delta meets threshold. */
    pd_sweep_feature_t feature;
} pd_sweep_alert_t;

int pd_analysis_sweep_alert(const pd_analysis_sweep_t *sweep, u32 selected_mask,
                            s32 delta_threshold_permille,
                            pd_sweep_alert_t *alert);

#endif /* PD_ANALYSIS_H */
