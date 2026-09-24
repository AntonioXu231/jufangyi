/* PS-only persistent spectrum-analysis archive. */
#include "pd_analysis.h"

#include "xil_cache.h"
#include "xstatus.h"
#include <string.h>

#define PD_ANALYSIS_MAGIC   0x5044414EU /* "PDAN" */
#define PD_ANALYSIS_VERSION 1U

static pd_analysis_record_t s_record[PD_ANALYSIS_ARCHIVE_COUNT];
static u32 s_next_sequence;
static pd_analysis_sweep_t s_sweep[PD_ANALYSIS_ARCHIVE_COUNT];
static u32 s_next_sweep_sequence;

static int record_is_current(const pd_analysis_record_t *record)
{
    if (record->magic != PD_ANALYSIS_MAGIC ||
        record->version != PD_ANALYSIS_VERSION ||
        record->analysis_sequence >= s_next_sequence)
        return 0;
    return (s_next_sequence - record->analysis_sequence) <= PD_ANALYSIS_ARCHIVE_COUNT;
}

static int sweep_is_current(const pd_analysis_sweep_t *sweep)
{
    if (sweep->magic != PD_ANALYSIS_MAGIC ||
        sweep->version != PD_ANALYSIS_VERSION ||
        sweep->sweep_sequence >= s_next_sweep_sequence)
        return 0;
    return (s_next_sweep_sequence - sweep->sweep_sequence) <= PD_ANALYSIS_ARCHIVE_COUNT;
}

static u32 sweep_start(u32 center_start, u32 ordinal, u32 total_samples)
{
    static const s32 offset[PD_ANALYSIS_SWEEP_WINDOWS] = {
        -1024, -512, 0, 512, 1024
    };
    s32 candidate = (s32)center_start + offset[ordinal];
    s32 last_start = (s32)(total_samples - PD_FFT_POINTS);
    if (candidate < 0) return 0U;
    if (candidate > last_start) return (u32)last_start;
    return (u32)candidate;
}

static u64 mean_pair_u64(u64 first, u64 second)
{
    return (first >> 1) + (second >> 1) + ((first & 1U) & (second & 1U));
}

static s32 delta_permille(u64 value, u64 reference)
{
    u64 magnitude;
    if (reference == 0U) return 0;
    if (value >= reference) {
        magnitude = ((value - reference) * 1000U) / reference;
        return magnitude > 0x7FFFFFFFULL ? 0x7FFFFFFF : (s32)magnitude;
    }
    magnitude = ((reference - value) * 1000U) / reference;
    return magnitude > 0x7FFFFFFFULL ? (s32)0x80000000U : -(s32)magnitude;
}

int pd_analysis_run(const pd_snapshot_record_t *snapshot, u32 snapshot_index,
                    u32 auto_window, u32 start_sample, u32 sample_rate_hz,
                    pd_analysis_record_t *result)
{
    pd_analysis_record_t saved;
    UINTPTR address;
    u32 total_samples;
    u32 slot;

    if (snapshot == 0 || result == 0 || sample_rate_hz == 0U ||
        snapshot_index >= PD_ANALYSIS_ARCHIVE_COUNT || snapshot->bytes == 0U ||
        (snapshot->bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U)
        return XST_FAILURE;

    total_samples = snapshot->bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    if (total_samples < PD_FFT_POINTS)
        return XST_FAILURE;
    address = (UINTPTR)snapshot->archive_addr;
    /* The hardware and PS archive copy completed before a snapshot record is published. */
    Xil_DCacheInvalidateRange(address, snapshot->bytes);

    (void)memset(&saved, 0, sizeof(saved));
    saved.magic = PD_ANALYSIS_MAGIC;
    saved.version = PD_ANALYSIS_VERSION;
    saved.analysis_sequence = s_next_sequence;
    saved.snapshot_sequence = snapshot->sequence;
    saved.snapshot_index = snapshot_index;
    saved.snapshot_bytes = snapshot->bytes;
    saved.window_auto = auto_window != 0U ? 1U : 0U;

    if (auto_window != 0U) {
        if (pd_spectrum_find_peak_window((const void *)address, snapshot->bytes,
                                         &saved.window) != XST_SUCCESS)
            return XST_FAILURE;
        start_sample = saved.window.start_sample;
    } else {
        if (start_sample > total_samples || PD_FFT_POINTS > total_samples - start_sample)
            return XST_FAILURE;
        saved.window.start_sample = start_sample;
        saved.window.peak_sample = 0U;
        saved.window.peak_channel = 0U;
        saved.window.raw_code = 0U;
        saved.window.delta_from_mid = 0U;
    }

    if (pd_spectrum_analyze((const void *)address, snapshot->bytes, start_sample,
                            sample_rate_hz, &saved.spectrum) != XST_SUCCESS)
        return XST_FAILURE;

    slot = s_next_sequence % PD_ANALYSIS_ARCHIVE_COUNT;
    s_record[slot] = saved;
    ++s_next_sequence;
    *result = saved;
    return XST_SUCCESS;
}

int pd_analysis_get(u32 index, pd_analysis_record_t *result)
{
    if (result == 0 || index >= PD_ANALYSIS_ARCHIVE_COUNT ||
        !record_is_current(&s_record[index]))
        return XST_FAILURE;
    *result = s_record[index];
    return XST_SUCCESS;
}

int pd_analysis_get_by_snapshot_sequence(u32 snapshot_sequence,
                                         pd_analysis_record_t *result)
{
    u32 index, found = 0U;
    pd_analysis_record_t newest;
    for (index = 0U; index < PD_ANALYSIS_ARCHIVE_COUNT; ++index) {
        if (record_is_current(&s_record[index]) &&
            s_record[index].snapshot_sequence == snapshot_sequence) {
            if (found == 0U || s_record[index].analysis_sequence > newest.analysis_sequence) {
                newest = s_record[index];
                found = 1U;
            }
        }
    }
    if (found == 0U) return XST_FAILURE;
    *result = newest;
    return XST_SUCCESS;
}

u32 pd_analysis_next_sequence(void)
{
    return s_next_sequence;
}

int pd_analysis_sweep_run(const pd_snapshot_record_t *snapshot, u32 snapshot_index,
                          u32 sample_rate_hz, pd_analysis_sweep_t *result)
{
    pd_analysis_sweep_t saved;
    UINTPTR address;
    u32 total_samples, ordinal, slot;

    if (snapshot == 0 || result == 0 || sample_rate_hz == 0U ||
        snapshot_index >= PD_ANALYSIS_ARCHIVE_COUNT || snapshot->bytes == 0U ||
        (snapshot->bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U)
        return XST_FAILURE;
    total_samples = snapshot->bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    if (total_samples < PD_FFT_POINTS) return XST_FAILURE;

    address = (UINTPTR)snapshot->archive_addr;
    Xil_DCacheInvalidateRange(address, snapshot->bytes);
    (void)memset(&saved, 0, sizeof(saved));
    saved.magic = PD_ANALYSIS_MAGIC;
    saved.version = PD_ANALYSIS_VERSION;
    saved.sweep_sequence = s_next_sweep_sequence;
    saved.snapshot_sequence = snapshot->sequence;
    saved.snapshot_index = snapshot_index;
    saved.snapshot_bytes = snapshot->bytes;
    if (pd_spectrum_find_peak_window((const void *)address, snapshot->bytes,
                                     &saved.event) != XST_SUCCESS)
        return XST_FAILURE;

    for (ordinal = 0U; ordinal < PD_ANALYSIS_SWEEP_WINDOWS; ++ordinal) {
        saved.start_sample[ordinal] = sweep_start(saved.event.start_sample, ordinal,
                                                   total_samples);
        if (pd_spectrum_analyze((const void *)address, snapshot->bytes,
                                saved.start_sample[ordinal], sample_rate_hz,
                                &saved.spectrum[ordinal]) != XST_SUCCESS)
            return XST_FAILURE;
    }
    slot = s_next_sweep_sequence % PD_ANALYSIS_ARCHIVE_COUNT;
    s_sweep[slot] = saved;
    ++s_next_sweep_sequence;
    *result = saved;
    return XST_SUCCESS;
}

int pd_analysis_sweep_get(u32 index, pd_analysis_sweep_t *result)
{
    if (result == 0 || index >= PD_ANALYSIS_ARCHIVE_COUNT ||
        !sweep_is_current(&s_sweep[index]))
        return XST_FAILURE;
    *result = s_sweep[index];
    return XST_SUCCESS;
}

int pd_analysis_sweep_get_by_snapshot_sequence(u32 snapshot_sequence,
                                               pd_analysis_sweep_t *result)
{
    u32 index, found = 0U;
    pd_analysis_sweep_t newest;
    for (index = 0U; index < PD_ANALYSIS_ARCHIVE_COUNT; ++index) {
        if (sweep_is_current(&s_sweep[index]) &&
            s_sweep[index].snapshot_sequence == snapshot_sequence) {
            if (found == 0U || s_sweep[index].sweep_sequence > newest.sweep_sequence) {
                newest = s_sweep[index];
                found = 1U;
            }
        }
    }
    if (found == 0U) return XST_FAILURE;
    *result = newest;
    return XST_SUCCESS;
}

u32 pd_analysis_sweep_next_sequence(void)
{
    return s_next_sweep_sequence;
}

int pd_analysis_sweep_feature(const pd_analysis_sweep_t *sweep,
                              pd_sweep_feature_t *feature)
{
    u32 channel;
    if (sweep == 0 || feature == 0 || !sweep_is_current(sweep))
        return XST_FAILURE;

    (void)memset(feature, 0, sizeof(*feature));
    feature->sweep_sequence = sweep->sweep_sequence;
    feature->snapshot_sequence = sweep->snapshot_sequence;
    feature->snapshot_index = sweep->snapshot_index;
    feature->event = sweep->event;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
        pd_sweep_feature_channel_t *out = &feature->channel[channel];
        out->pre_band_power = mean_pair_u64(sweep->spectrum[0].channel[channel].band_power,
                                            sweep->spectrum[1].channel[channel].band_power);
        out->event_band_power = sweep->spectrum[2].channel[channel].band_power;
        out->post_band_power = mean_pair_u64(sweep->spectrum[3].channel[channel].band_power,
                                             sweep->spectrum[4].channel[channel].band_power);
        out->event_delta_permille = delta_permille(out->event_band_power,
                                                   out->pre_band_power);
        out->recovery_permille = delta_permille(out->post_band_power,
                                                out->pre_band_power);
    }
    return XST_SUCCESS;
}

int pd_analysis_sweep_alert(const pd_analysis_sweep_t *sweep, u32 selected_mask,
                            s32 delta_threshold_permille,
                            pd_sweep_alert_t *alert)
{
    u32 channel;
    if (alert == 0 || pd_analysis_sweep_feature(sweep, &alert->feature) != XST_SUCCESS)
        return XST_FAILURE;
    alert->selected_mask = selected_mask & 0xFU;
    alert->delta_threshold_permille = delta_threshold_permille;
    alert->hit_mask = 0U;
    /* A nonpositive threshold explicitly disables verdict generation. */
    if (delta_threshold_permille <= 0) return XST_SUCCESS;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
        if ((alert->selected_mask & (1U << channel)) != 0U &&
            alert->feature.channel[channel].event_delta_permille >=
                delta_threshold_permille)
            alert->hit_mask |= (1U << channel);
    }
    return XST_SUCCESS;
}

void pd_analysis_clear(void)
{
    (void)memset(s_record, 0, sizeof(s_record));
    (void)memset(s_sweep, 0, sizeof(s_sweep));
    s_next_sequence = 0U;
    s_next_sweep_sequence = 0U;
}
