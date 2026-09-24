/* Raw snapshot unpacking and sanity statistics; see pd_snapshot_unpack.h. */
#include "pd_snapshot_unpack.h"

#include "xstatus.h"

static void decode_sample(const u8 *p, u16 sample[PD_SNAPSHOT_CHANNELS])
{
    /* p[0] is bit 7:0 of the 48-bit sample word in DDR memory. */
    sample[0] = (u16)p[0] | ((u16)(p[1] & 0x0FU) << 8);
    sample[1] = ((u16)p[1] >> 4) | ((u16)p[2] << 4);
    sample[2] = (u16)p[3] | ((u16)(p[4] & 0x0FU) << 8);
    sample[3] = ((u16)p[4] >> 4) | ((u16)p[5] << 4);
}

static int validate_raw(const void *raw, u32 bytes, u32 *samples)
{
    if (raw == 0 || samples == 0 || bytes == 0U ||
        (bytes % PD_SNAPSHOT_BLOCK_BYTES) != 0U)
        return XST_FAILURE;
    *samples = bytes / PD_SNAPSHOT_BYTES_PER_SAMPLE;
    return XST_SUCCESS;
}

int pd_snapshot_unpack(const void *raw, u32 bytes, pd_waveform_t *waveform)
{
    const u8 *p = (const u8 *)raw;
    u16 sample[PD_SNAPSHOT_CHANNELS];
    u32 i, channel, samples;

    if (waveform == 0 || validate_raw(raw, bytes, &samples) != XST_SUCCESS ||
        waveform->capacity_samples < samples)
        return XST_FAILURE;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel)
        if (waveform->channel[channel] == 0) return XST_FAILURE;

    for (i = 0U; i < samples; ++i, p += PD_SNAPSHOT_BYTES_PER_SAMPLE) {
        decode_sample(p, sample);
        for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel)
            waveform->channel[channel][i] = sample[channel];
    }
    waveform->sample_count = samples;
    return XST_SUCCESS;
}

int pd_snapshot_read_sample(const void *raw, u32 bytes, u32 sample_index,
                            u16 sample[PD_SNAPSHOT_CHANNELS])
{
    u32 samples;
    if (sample == 0 || validate_raw(raw, bytes, &samples) != XST_SUCCESS ||
        sample_index >= samples)
        return XST_FAILURE;
    decode_sample((const u8 *)raw + sample_index * PD_SNAPSHOT_BYTES_PER_SAMPLE,
                  sample);
    return XST_SUCCESS;
}

int pd_snapshot_measure(const void *raw, u32 bytes, pd_snapshot_stats_t *stats)
{
    const u8 *p = (const u8 *)raw;
    u16 sample[PD_SNAPSHOT_CHANNELS];
    u64 sum[PD_SNAPSHOT_CHANNELS] = {0U, 0U, 0U, 0U};
    u32 i, channel, samples;

    if (stats == 0 || validate_raw(raw, bytes, &samples) != XST_SUCCESS)
        return XST_FAILURE;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
        stats->minimum[channel] = 0x0FFFU;
        stats->maximum[channel] = 0U;
    }
    for (i = 0U; i < samples; ++i, p += PD_SNAPSHOT_BYTES_PER_SAMPLE) {
        decode_sample(p, sample);
        for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel) {
            if (sample[channel] < stats->minimum[channel]) stats->minimum[channel] = sample[channel];
            if (sample[channel] > stats->maximum[channel]) stats->maximum[channel] = sample[channel];
            sum[channel] += sample[channel];
        }
    }
    stats->sample_count = samples;
    for (channel = 0U; channel < PD_SNAPSHOT_CHANNELS; ++channel)
        stats->mean[channel] = (u32)(sum[channel] / samples);
    return XST_SUCCESS;
}
