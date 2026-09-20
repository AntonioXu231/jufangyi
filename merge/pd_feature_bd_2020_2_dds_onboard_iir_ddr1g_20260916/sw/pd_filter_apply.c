/*
 * Safe IIR configuration sequence for pd_filter_chain.
 * Run only after exporting an XSA whose filter base is 0x40020000.
 * The program leaves the filter bypassed on every verification failure.
 */
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"

#define FILTER_BASE 0x40020000U
#define FCTRL 0x000U
#define FSTATUS 0x004U
#define FBYPASS_MASK 0x008U
#define FSAMPLE_HZ 0x00CU
#define CH_STRIDE 0x200U
#define COEF_BASE 0x014U
#define COEF_STRIDE 0x004U
#define FILTER_VERSION 0x04U

/* 26 MSPS, 100 kHz..1 MHz, Q1.16 values in the low 18 bits. */
static const unsigned coef[5] = { 0x01916U, 0x00000U, 0x3E6EAU, 0x23385U, 0x0CDD4U };

static int check(unsigned off, unsigned expect)
{
    unsigned got = Xil_In32(FILTER_BASE + off);
    if (got != expect) {
        xil_printf("FAIL: readback off=0x%03x got=0x%08x exp=0x%08x\r\n", off, got, expect);
        return 0;
    }
    return 1;
}

static int safe_bypass(void)
{
    Xil_Out32(FILTER_BASE + FCTRL, 1U);
    Xil_Out32(FILTER_BASE + FBYPASS_MASK, 0xFU);
    return check(FCTRL, 1U) && check(FBYPASS_MASK, 0xFU);
}

int main(void)
{
    unsigned ch, i, off, status;
    xil_printf("--- pd_filter_apply 26 MSPS ---\r\n");

    if (!safe_bypass()) return XST_FAILURE;
    if (Xil_In32(FILTER_BASE + FSAMPLE_HZ) != 26000000U) {
        xil_printf("FAIL: unexpected sample rate\r\n");
        return XST_FAILURE;
    }

    for (ch = 0; ch < 4; ++ch) {
        off = 0x010U + ch*CH_STRIDE;
        Xil_Out32(FILTER_BASE + off, 0U); /* shadow enable off */
        for (i = 0; i < 5; ++i) {
            unsigned woff = COEF_BASE + ch*CH_STRIDE + i*COEF_STRIDE;
            Xil_Out32(FILTER_BASE + woff, coef[i]);
            if (!check(woff, coef[i])) { safe_bypass(); return XST_FAILURE; }
        }
    }

    status = Xil_In32(FILTER_BASE + FSTATUS);
    if (((status >> 8) & 0xffU) != FILTER_VERSION || !(status & 0x2U)) {
        xil_printf("FAIL: coefficient dirty/version status=0x%08x\r\n", status);
        safe_bypass(); return XST_FAILURE;
    }

    for (ch = 0; ch < 4; ++ch)
        Xil_Out32(FILTER_BASE + 0x010U + ch*CH_STRIDE, 1U);

    Xil_Out32(FILTER_BASE + FCTRL, 0x3U); /* keep global bypass, APPLY */
    status = Xil_In32(FILTER_BASE + FSTATUS);
    if (status & 0x2U) {
        xil_printf("FAIL: APPLY did not clear coef_dirty, status=0x%08x\r\n", status);
        safe_bypass(); return XST_FAILURE;
    }

    Xil_Out32(FILTER_BASE + FCTRL, 0U);
    Xil_Out32(FILTER_BASE + FBYPASS_MASK, 0U);
    status = Xil_In32(FILTER_BASE + FSTATUS);
    if (((status >> 8) & 0xffU) != FILTER_VERSION) {
        xil_printf("FAIL: post-apply version status=0x%08x\r\n", status);
        safe_bypass(); return XST_FAILURE;
    }

    xil_printf("FILTER_APPLY_VERIFY_PASS\r\n");
    return XST_SUCCESS;
}
