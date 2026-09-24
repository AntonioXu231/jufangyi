/*
 * pd_hw_map.h -- hardware contract shared by every PS-side service module.
 *
 * Keep AXI-Lite offsets and DDR ownership here.  This header deliberately
 * mirrors the proven v1.8.0 capture service; it does not alter PL behaviour.
 */
#ifndef PD_HW_MAP_H
#define PD_HW_MAP_H

#include "xparameters.h"
#include "xil_types.h"

#if defined(XPAR_PD_DDR_0_BASEADDR)
# define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
#elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
# define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
#else
# error "Cannot find pd_ddr AXI-Lite base address in xparameters.h"
#endif

#if defined(XPAR_PD_FEATURE_0_BASEADDR)
# define PD_FEATURE_BASE ((UINTPTR)XPAR_PD_FEATURE_0_BASEADDR)
#elif defined(XPAR_PD_FEATURE_SYS_TOP_0_BASEADDR)
# define PD_FEATURE_BASE ((UINTPTR)XPAR_PD_FEATURE_SYS_TOP_0_BASEADDR)
#else
# error "Cannot find pd_feature AXI-Lite base address in xparameters.h"
#endif

/* pd_feature channel i occupies a 0x80-byte register page. */
#define PD_FEATURE_CFG0(ch)   ((u32)(ch) * 0x80U + 0x04U)
#define PD_FEATURE_STATUS(ch) ((u32)(ch) * 0x80U + 0x20U)

#if defined(XPAR_XAXIDMA_0_BASEADDR)
# define PD_DMA_BASE       ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
# define PD_DMA_LOOKUP_ARG XPAR_XAXIDMA_0_BASEADDR
#elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
# define PD_DMA_BASE       ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
# define PD_DMA_LOOKUP_ARG XPAR_AXIDMA_0_DEVICE_ID
#else
# error "No AXI-DMA base macro: regenerate the Vitis platform from the matching XSA."
#endif

#if defined(XPAR_PS7_DDR_0_BASEADDRESS)
# define PD_PS_DDR_BASE XPAR_PS7_DDR_0_BASEADDRESS
#elif defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR)
# define PD_PS_DDR_BASE XPAR_PS7_DDR_0_S_AXI_BASEADDR
#else
# error "No PS DDR base macro in xparameters.h"
#endif

/* pd_ddr AXI-Lite offsets. */
#define PD_DDR_CTRL              0x000U
#define PD_DDR_STATUS            0x004U
#define PD_DDR_RING_BASE          0x008U
#define PD_DDR_RING_SIZE          0x00CU
#define PD_DDR_RING_WR_PTR        0x010U
#define PD_FREEZE_CTRL           0x034U
#define PD_SLOT_CTRL             0x04CU
#define PD_SLOT_STATUS           0x050U
#define PD_SLOT_SEQ              0x054U
#define PD_SLOT_DROPS            0x058U
#define PD_SNAP_TRIG_CTRL        0x05CU
#define PD_SLOT_BASE_OFF(n)      (0x060U + ((n) * 0x10U))
#define PD_SLOT_LEN_OFF(n)       (0x064U + ((n) * 0x10U))
#define PD_SLOT_SEQ_OFF(n)       (0x068U + ((n) * 0x10U))
#define PD_SLOT_FLAGS_OFF(n)     (0x06CU + ((n) * 0x10U))

#define PD_DDR_COPY_BUSY         (1U << 1)
#define PD_DDR_RING_ERR          (1U << 5)
#define PD_DDR_COPY_ERR          (1U << 8)
#define PD_DDR_CFG_ERR           (1U << 9)
#define PD_SLOT_VALID_MASK       0x0000000FU
#define PD_SLOT_BUSY_MASK        0x000000F0U
#define PD_SLOT_LOCKED_MASK      0x00000F00U
#define PD_SLOT_CFG_ERR          (1U << 13)
#define PD_SLOT_REQ_OVERFLOW     (1U << 14)
#define PD_SLOT_CMD_ERR          (1U << 15)
#define PD_SLOT_LAST_SHIFT       17U
#define PD_SLOT_READY            (1U << 19)
#define PD_TRIG_ENABLED          (1U << 0)
#define PD_TRIG_MASK_ALL         (0xFU << 1)
#define PD_TRIG_ARMED            (1U << 16)

/* AXI DMA S2MM register bank. */
#define PD_S2MM_DMACR_OFFSET     0x30U
#define PD_S2MM_DMASR_OFFSET     0x34U
#define PD_S2MM_LENGTH_OFFSET    0x58U
#define PD_DMASR_HALTED          0x00000001U
#define PD_DMASR_IDLE            0x00000002U
#define PD_DMASR_ERROR_MASK      0x00004070U

/* PS DDR allocation.  Do not overlap the PL ring or hardware snapshot slots. */
#define PD_RX_BUFFER_BASE        ((UINTPTR)PD_PS_DDR_BASE + 0x01000000U)
#define PD_RX_BUFFER_BYTES       65528U
#define PD_EVENT_ARCHIVE_BASE    0x27000000U
#define PD_EVENT_ARCHIVE_STRIDE  0x00010000U
#define PD_SNAP_ARCHIVE_BASE     0x24000000U
#define PD_SNAP_ARCHIVE_STRIDE   0x00C00000U
#define PD_SNAP_SLOT_LOW         0x20001000U
#define PD_SNAP_SLOT_HIGH        0x23001000U

#endif /* PD_HW_MAP_H */
