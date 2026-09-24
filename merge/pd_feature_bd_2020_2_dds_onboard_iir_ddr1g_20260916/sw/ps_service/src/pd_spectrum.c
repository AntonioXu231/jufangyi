/* Fixed-point 1024-point FFT for the standalone Cortex-A9 acquisition service. */
#include "pd_spectrum.h"

#include "xstatus.h"

typedef struct {
    s32 re;
    s32 im;
} pd_complex_q15_t;

/* sin(pi*n/512), n=0..256, in Q15.  Quadrant mapping yields sin(2*pi*n/1024). */
static const s16 s_sin_quarter_q15[257] = {
     0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210, 2410, 2611, 2811, 3012,
     3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609, 4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195,
     6393, 6590, 6786, 6983, 7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
     9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353,
     12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828, 14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
     15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
     18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631,
     20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856, 22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027,
     23170, 23311, 23452, 23592, 23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
     25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
     27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001, 28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803,
     28898, 28992, 29085, 29177, 29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
     30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297,
     31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736, 31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098,
     32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
     32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752, 32757, 32761, 32765, 32766,
     32767
};

static s32 q15_mul(s32 a, s32 b)
{
    s64 product = (s64)a * b;
    if (product >= 0) return (s32)((product + (1LL << 14)) >> 15);
    return -(s32)(((-product) + (1LL << 14)) >> 15);
}

static s32 sin_turn_1024(u32 turn)
{
    u32 quadrant;
    u32 position;
    turn &= (PD_FFT_POINTS - 1U);
    quadrant = turn >> 8;
    position = turn & 0xFFU;
    if (quadrant == 0U) return s_sin_quarter_q15[position];
    if (quadrant == 1U) return s_sin_quarter_q15[256U - position];
    if (quadrant == 2U) return -s_sin_quarter_q15[position];
    return -s_sin_quarter_q15[256U - position];
}

static s32 hann_q15(u32 sample)
{
    /* Periodic Hann: 0.5 * (1 - cos(2*pi*n/N)); appropriate for an N-point FFT. */
    return (32767 - sin_turn_1024(sample + 256U)) >> 1;
}

static void bit_reverse(pd_complex_q15_t data[PD_FFT_POINTS])
{
    u32 i, j = 0U, bit;
    pd_complex_q15_t swap;
    for (i = 1U; i < PD_FFT_POINTS; ++i) {
        bit = PD_FFT_POINTS >> 1;
        while ((j & bit) != 0U) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) { swap = data[i]; data[i] = data[j]; data[j] = swap; }
    }
}

static void fft_forward(pd_complex_q15_t data[PD_FFT_POINTS])
{
    u32 length, half, group, k, step, phase;
    s32 wr, wi, vr, vi, ur, ui;
    bit_reverse(data);
    for (length = 2U; length <= PD_FFT_POINTS; length <<= 1) {
        half = length >> 1;
        step = PD_FFT_POINTS / length;
        for (group = 0U; group < PD_FFT_POINTS; group += length) {
            for (k = 0U; k < half; ++k) {
                phase = k * step;
                wr = sin_turn_1024(phase + 256U);
                wi = -sin_turn_1024(phase);
                vr = q15_mul(data[group + k + half].re, wr) - q15_mul(data[group + k + half].im, wi);
                vi = q15_mul(data[group + k + half].re, wi) + q15_mul(data[group + k + half].im, wr);
                ur = data[group + k].re;
                ui = data[group + k].im;
                /* One bit per stage prevents overflow and normalizes the DFT by N. */
                data[group + k].re        = (ur + vr) >> 1;
                data[group + k].im        = (ui + vi) >> 1;
                data[group + k + half].re = (ur - vr) >> 1;
                data[group + k + half].im = (ui - vi) >> 1;
            }
        }
    }
}

static u32 isqrt_u64(u64 value)
{
    u64 result = 0U;
    u64 bit = 1ULL << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (u32)result;
}

int pd_spectrum_analyze(const void *raw, u32 bytes, u32 start_sample,
                        u32 sample_rate_hz, pd_spectrum_result_t *result)
{
    pd_complex_q15_t data[PD_FFT_POINTS];
    u16 sample[PD_SNAPSHOT_CHANNELS];
    s64 sum;
    u64 power, band_power;
    u32 total_samples, channel, i, bin, peak_bin, peak_amplitude;
    s32 dc, amplitude;

    if (raw == 0 || result == 0 || sample_rate_hz == 0U ||
        bytes == 0U || (bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U)
        return XST_FAILURE;
    total_samples = bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    if (start_sample > total_samples || PD_FFT_POINTS > total_samples - start_sample)
        return XST_FAILURE;

    result->sample_rate_hz = sample_rate_hz;
    result->start_sample = start_sample;
    result->fft_points = PD_FFT_POINTS;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
        sum = 0;
        for (i = 0U; i < PD_FFT_POINTS; ++i) {
            if (pd_snapshot_read_sample(raw, bytes, start_sample + i, sample) != XST_SUCCESS)
                return XST_FAILURE;
            sum += sample[channel];
        }
        dc = (s32)(sum / PD_FFT_POINTS);
        result->channel[channel].dc_code = (u32)dc;

        for (i = 0U; i < PD_FFT_POINTS; ++i) {
            (void)pd_snapshot_read_sample(raw, bytes, start_sample + i, sample);
            data[i].re = q15_mul((s32)sample[channel] - dc, hann_q15(i));
            data[i].im = 0;
        }
        fft_forward(data);
        peak_bin = 1U;
        peak_amplitude = 0U;
        band_power = 0U;
        for (bin = 1U; bin <= PD_FFT_POINTS / 2U; ++bin) {
            power = (u64)((s64)data[bin].re * data[bin].re) +
                    (u64)((s64)data[bin].im * data[bin].im);
            band_power += power;
            amplitude = (s32)isqrt_u64(power) * 4;
            if ((u32)amplitude > peak_amplitude) {
                peak_amplitude = (u32)amplitude;
                peak_bin = bin;
            }
        }
        result->channel[channel].peak_bin = peak_bin;
        result->channel[channel].peak_hz =
            (u32)(((u64)peak_bin * sample_rate_hz) / PD_FFT_POINTS);
        result->channel[channel].amplitude_code = peak_amplitude;
        result->channel[channel].band_power = band_power;
    }
    return XST_SUCCESS;
}

int pd_spectrum_find_peak_window(const void *raw, u32 bytes,
                                 pd_spectrum_window_t *window)
{
    u16 sample[PD_SNAPSHOT_CHANNELS];
    u32 total_samples, i, channel, delta, best_delta = 0U;

    if (raw == 0 || window == 0 || bytes == 0U ||
        (bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U)
        return XST_FAILURE;
    total_samples = bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    if (total_samples < PD_FFT_POINTS) return XST_FAILURE;

    window->peak_sample = 0U;
    window->peak_channel = 0U;
    window->raw_code = 0U;
    for (i = 0U; i < total_samples; ++i) {
        if (pd_snapshot_read_sample(raw, bytes, i, sample) != XST_SUCCESS)
            return XST_FAILURE;
        for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
            delta = sample[channel] >= 2048U ? sample[channel] - 2048U :
                                                2048U - sample[channel];
            if (delta > best_delta) {
                best_delta = delta;
                window->peak_sample = i;
                window->peak_channel = channel;
                window->raw_code = sample[channel];
            }
        }
    }
    window->delta_from_mid = best_delta;
    window->start_sample = window->peak_sample > (PD_FFT_POINTS / 2U) ?
                           window->peak_sample - (PD_FFT_POINTS / 2U) : 0U;
    if (window->start_sample > total_samples - PD_FFT_POINTS)
        window->start_sample = total_samples - PD_FFT_POINTS;
    return XST_SUCCESS;
}

int pd_spectrum_self_test(pd_spectrum_result_t *result)
{
    static u8 raw[PD_FFT_POINTS * PD_SNAPSHOT_BYTES_PER_SAMPLE];
    static const u32 expected_bin[PD_SNAPSHOT_CHANNELS] = {37U, 83U, 151U, 255U};
    u16 value[PD_SNAPSHOT_CHANNELS];
    u32 i, channel, phase;

    if (result == 0) return XST_FAILURE;
    for (i = 0U; i < PD_FFT_POINTS; ++i) {
        for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
            phase = (i * expected_bin[channel]) & (PD_FFT_POINTS - 1U);
            value[channel] = (u16)(2048 + (q15_mul(1024, sin_turn_1024(phase))));
        }
        raw[i * 6U + 0U] = (u8)value[0];
        raw[i * 6U + 1U] = (u8)(((value[0] >> 8) & 0x0FU) | ((value[1] & 0x0FU) << 4));
        raw[i * 6U + 2U] = (u8)(value[1] >> 4);
        raw[i * 6U + 3U] = (u8)value[2];
        raw[i * 6U + 4U] = (u8)(((value[2] >> 8) & 0x0FU) | ((value[3] & 0x0FU) << 4));
        raw[i * 6U + 5U] = (u8)(value[3] >> 4);
    }
    if (pd_spectrum_analyze(raw, sizeof(raw), 0U,
                            PD_SPECTRUM_DEFAULT_FS_HZ, result) != XST_SUCCESS)
        return XST_FAILURE;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
        if (result->channel[channel].peak_bin != expected_bin[channel] ||
            result->channel[channel].amplitude_code < 900U ||
            result->channel[channel].amplitude_code > 1150U)
            return XST_FAILURE;
    }
    return XST_SUCCESS;
}
