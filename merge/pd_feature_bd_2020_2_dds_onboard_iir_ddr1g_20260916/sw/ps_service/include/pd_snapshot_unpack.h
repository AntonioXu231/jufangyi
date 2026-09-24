/* Raw DDR snapshot decoder.
 *
 * PL storage contract (pd_pack48.v + pd_pack192.v):
 *   one sample instant = 48 bits, {ch3[11:0],ch2[11:0],ch1[11:0],ch0[11:0]};
 *   four instants = one 24-byte block; bytes in DDR are little endian.
 * Each channel value is ADC offset-binary (0x800 is the nominal zero level).
 */
#ifndef PD_SNAPSHOT_UNPACK_H
#define PD_SNAPSHOT_UNPACK_H

#include "xil_types.h"

#define PD_SNAPSHOT_CHANNELS          4U
#define PD_SNAPSHOT_ADC_BITS          12U
#define PD_SNAPSHOT_BYTES_PER_SAMPLE  6U
#define PD_SNAPSHOT_BLOCK_BYTES       24U

/* Caller owns all four destination buffers; no heap is used in bare metal. */
typedef struct {
    u32 capacity_samples;
    u32 sample_count;
    u16 *channel[PD_SNAPSHOT_CHANNELS];
} pd_waveform_t;

typedef struct {
    u32 sample_count;
    u16 minimum[PD_SNAPSHOT_CHANNELS];
    u16 maximum[PD_SNAPSHOT_CHANNELS];
    u32 mean[PD_SNAPSHOT_CHANNELS];
} pd_snapshot_stats_t;

/* Decode all channels from a complete PL snapshot into caller-provided buffers. */
int pd_snapshot_unpack(const void *raw, u32 bytes, pd_waveform_t *waveform);

/* Read one four-channel sample without allocating a full waveform buffer. */
int pd_snapshot_read_sample(const void *raw, u32 bytes, u32 sample_index,
                            u16 sample[PD_SNAPSHOT_CHANNELS]);

/* Decode and reduce a complete snapshot without allocating waveform buffers. */
int pd_snapshot_measure(const void *raw, u32 bytes, pd_snapshot_stats_t *stats);

#endif /* PD_SNAPSHOT_UNPACK_H */
