/* PS-2 TCP application entry.  Import this OR ../src/main.c, never both. */
#include "pd_acquisition.h"
#include "pd_tcp_service.h"

#include "xil_printf.h"
#include "xstatus.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "platform.h"
#include "platform_config.h"

/* Required by the Xilinx lwIP RAW-mode template and pd_tcp_service.c. */
struct netif echo_netif;

int main(void)
{
    ip_addr_t ipaddr, netmask, gw;
    unsigned char mac[6] = { 0x00U, 0x0AU, 0x35U, 0x00U, 0x01U, 0x02U };

    init_platform();
    IP4_ADDR(&ipaddr, 192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 1, 1);
    lwip_init();
    if (xemac_add(&echo_netif, &ipaddr, &netmask, &gw, mac,
                  PLATFORM_EMAC_BASEADDR) == NULL) {
        xil_printf("TCP_PS2_FAIL: xemac_add failed\r\n");
        for (;;) __asm__ volatile ("wfi");
    }
    netif_set_default(&echo_netif);
#ifndef SDT
    platform_enable_interrupts();
#endif
    netif_set_up(&echo_netif);
    if (pd_acq_init() != XST_SUCCESS || pd_tcp_service_init() != XST_SUCCESS) {
        xil_printf("TCP_PS2_FAIL: core=%s\r\n", pd_acq_last_error());
        for (;;) __asm__ volatile ("wfi");
    }
    pd_acq_set_poll_hook(pd_tcp_service_poll);
    xil_printf("\r\n--- pd modular TCP acquisition service PS-2 ---\r\n");
    xil_printf("IP=192.168.1.10 TCP=6001; connect and send HELP\r\n");
    for (;;) (void)pd_acq_poll();
}
