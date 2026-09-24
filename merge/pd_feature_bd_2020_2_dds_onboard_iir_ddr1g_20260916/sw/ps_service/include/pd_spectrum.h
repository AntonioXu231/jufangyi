/* Four-channel, PS-side 1024-point spectrum analysis of a raw DDR snapshot. */
#ifndef PD_SPECTRUM_H
#define PD_SPECTRUM_H

#include "xil_types.h"
#include "pd_snapshot_unpack.h"

#define PD_FFT_POINTS                 1024U
#define PD_SPECTRUM_DEFAULT_FS_HZ  26000000U

typedef struct {
    u32 dc_code;          /* Mean raw offset-binary ADC code before removal. */
    u32 peak_bin;         /* 1..512; bin zero is deliberately excluded. */
    u32 peak_hz;          /* peak_bin * sample_rate_hz / 1024. */
    u32 amplitude_code;   /* Hann coherent-gain-corrected approximate peak code. */
    u64 band_power;       /* Sum of non-DC FFT-bin powers; relative-only, not calibrated. */
} pd_spectrum_channel_t;

typedef struct {
    u32 sample_rate_hz;
    u32 start_sample;
    u32 fft_points;
    pd_spectrum_channel_t channel[PD_SNAPSHOT_CHANNELS];
} pd_spectrum_result_t;

typedef struct {
    u32 start_sample;      /* 1024-point window start selected around peak_sample. */
    u32 peak_sample;       /* Index in the full raw snapshot. */
    u32 peak_channel;      /* 0..3. */
    u16 raw_code;          /* Original offset-binary code at the selected peak. */
    u32 delta_from_mid;    /* Absolute distance from nominal 0x800 code. */
} pd_spectrum_window_t;

/*
 * Decode raw snapshot data, remove each channel DC mean, apply a periodic Hann
 * window, perform a forward 1024-point FFT, and report the strongest non-DC bin.
 */
int pd_spectrum_analyze(const void *raw, u32 bytes, u32 start_sample,
                        u32 sample_rate_hz, pd_spectrum_result_t *result);

/* Select one common four-channel FFT window around the strongest raw excursion. */
int pd_spectrum_find_peak_window(const void *raw, u32 bytes,
                                 pd_spectrum_window_t *window);

/* Deterministic 4-tone decode/window/FFT/peak regression test. */
int pd_spectrum_self_test(pd_spectrum_result_t *result);

#endif /* PD_SPECTRUM_H */
