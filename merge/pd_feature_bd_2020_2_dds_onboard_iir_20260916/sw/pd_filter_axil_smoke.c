/*
 * Non-intrusive AXI4-Lite smoke test for pd_filter_0.
 *
 * This program does not arm DMA and does not clear/enable the filter.  It only
 * verifies that the newly added register aperture is reachable and that its
 * reset bypass state is preserved.  Add it as a separate Vitis application
 * after exporting the XSA generated from the filter-enabled BD.
 */
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"

#if defined(XPAR_PD_FILTER_0_S_AXI_BASEADDR)
# define PD_FILTER_BASE XPAR_PD_FILTER_0_S_AXI_BASEADDR
#elif defined(XPAR_PD_FILTER_0_BASEADDR)
# define PD_FILTER_BASE XPAR_PD_FILTER_0_BASEADDR
#else
/* Fixed BD allocation; the Vitis XSA must still be regenerated before use. */
# define PD_FILTER_BASE 0x40020000U
#endif

#define FCTRL        0x000U
#define FSTATUS      0x004U
#define FBYPASS_MASK 0x008U
#define FSAMPLE_HZ   0x00CU

#define FILTER_VERSION 0x04U

int main(void)
{
    u32 ctrl, status, mask, sample_hz;

    xil_printf("--- pd_filter AXI-Lite smoke test ---\r\n");
    xil_printf("base=0x%08lx\r\n", (unsigned long)PD_FILTER_BASE);

    ctrl      = Xil_In32(PD_FILTER_BASE + FCTRL);
    status    = Xil_In32(PD_FILTER_BASE + FSTATUS);
    mask      = Xil_In32(PD_FILTER_BASE + FBYPASS_MASK);
    sample_hz = Xil_In32(PD_FILTER_BASE + FSAMPLE_HZ);
    xil_printf("FCTRL=%08lx FSTATUS=%08lx FBYPASS=%08lx FSAMPLE=%lu\r\n",
               (unsigned long)ctrl, (unsigned long)status,
               (unsigned long)mask, (unsigned long)sample_hz);

    if ((ctrl & 1U) == 0U) {
        xil_printf("FAIL: global bypass is not asserted; do not run DMA regression.\r\n");
        return XST_FAILURE;
    }
    if ((mask & 0xFU) != 0xFU) {
        xil_printf("FAIL: per-channel bypass is not 0xF; do not run DMA regression.\r\n");
        return XST_FAILURE;
    }
    if (((status >> 8) & 0xFFU) != FILTER_VERSION) {
        xil_printf("FAIL: unexpected filter version.\r\n");
        return XST_FAILURE;
    }
    if (sample_hz != 26000000U) {
        xil_printf("FAIL: unexpected sample rate.\r\n");
        return XST_FAILURE;
    }

    xil_printf("PASS: AXI aperture responds; default IIR path is safely bypassed.\r\n");
    return XST_SUCCESS;
}
