/* Public PS acquisition-core interface. */
#ifndef PD_ACQUISITION_H
#define PD_ACQUISITION_H

#include "xil_types.h"

/* PS-owned circular archive geometry.  Front ends and analysis modules use it. */
#define PD_EVENT_ARCHIVE_COUNT 16U
#define PD_SNAP_ARCHIVE_COUNT  4U

typedef struct {
    u32 sequence;
    u32 ddr_addr;
    u32 bytes;
    u32 peak_words;
    u32 cycle_words;
} pd_event_record_t;

typedef struct {
    u32 sequence;
    u32 source_slot;
    u32 source_addr;
    u32 archive_addr;
    u32 bytes;
    u32 hw_slot_sequence;
    u32 flags;
} pd_snapshot_record_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 event_sequence;
    u32 snapshot_sequence;
    u32 event_overwrites;
    u32 snapshot_overwrites;
    u32 dma_errors;
    u32 slot_errors;
    u32 recovery_count;
    u32 recovery_discarded_slots;
    u32 last_dma_status;
    u32 last_ddr_status;
    u32 last_slot_status;
    pd_event_record_t event[PD_EVENT_ARCHIVE_COUNT];
    pd_snapshot_record_t snapshot[PD_SNAP_ARCHIVE_COUNT];
} pd_acq_shared_t;

typedef enum {
    PD_ACQ_IDLE = 0,
    PD_ACQ_RUNNING,
    PD_ACQ_STOPPING,
    PD_ACQ_FAULT
} pd_acq_state_t;

/* Called from the main loop while the core waits for hardware progress. */
typedef void (*pd_acq_poll_hook_t)(void);

extern volatile pd_acq_shared_t g_pd_acq;

int pd_acq_init(void);
int pd_acq_start(u32 packet_limit);
void pd_acq_request_stop(void);
int pd_acq_poll(void);
pd_acq_state_t pd_acq_state(void);
u32 pd_acq_packets(void);
/* Runtime default used by a START command without an explicit packet count. */
int pd_acq_set_default_packet_limit(u32 packet_limit);
u32 pd_acq_default_packet_limit(void);
/* FAULT only: discards unarchived PL slots but preserves PS DDR archives. */
int pd_acq_recover(u32 *discarded_slots);
/* Sequence lookup rejects records overwritten by the PS DDR archive ring. */
int pd_acq_get_event_by_sequence(u32 sequence, pd_event_record_t *record);
int pd_acq_get_snapshot_by_sequence(u32 sequence, pd_snapshot_record_t *record);
const char *pd_acq_last_error(void);
void pd_acq_set_poll_hook(pd_acq_poll_hook_t hook);
void pd_acq_clear_metadata(void);

#endif /* PD_ACQUISITION_H */
