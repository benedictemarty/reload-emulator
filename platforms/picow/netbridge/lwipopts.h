#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwipopts.h — lwIP of the Pico W network bridge: UDP and DHCP only, no OS
// (pico_cyw43_arch_lwip_poll : everything runs from the main loop)

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000
#define MEMP_NUM_UDP_PCB            4
#define PBUF_POOL_SIZE              16
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_UDP                    1
#define LWIP_TCP                    1   // Needed by the cyw43 lwIP glue (unused by the bridge)
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_DHCP                   1
#define LWIP_DNS                    0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
