#pragma once

#include <net/netmap_user.h>
#include <sys/queue.h>

#include "eth.h"
#include "ip.h"
#include "udp.h"
#include "warpcore.h"


// according to Luigi, any ring can be passed to NETMAP_BUF
#define IDX2BUF(w, i) NETMAP_BUF(NETMAP_TXRING((w)->nif, 0), (i))


struct w_sock {
    struct warpcore * w;        // warpcore instance
    STAILQ_HEAD(ivh, w_iov) iv; // iov for read data
    STAILQ_HEAD(ovh, w_iov) ov; // iov for data to write
    SLIST_ENTRY(w_sock) next;   // next socket

    struct {
        struct eth_hdr eth;
        struct ip_hdr ip __attribute__((packed));
        struct udp_hdr udp;
    } hdr;
    uint8_t _unused[6];
};


struct warpcore {
    struct netmap_if * nif;       // netmap interface
    struct w_sock ** udp;         // UDP "sockets"
    SLIST_HEAD(sh, w_sock) sock;  // our open sockets
    uint32_t cur_txr;             // our current tx ring
    uint32_t cur_rxr;             // our current rx ring
    STAILQ_HEAD(iovh, w_iov) iov; // our available bufs
    uint32_t ip;                  // our IP address
    uint32_t mask;                // our IP netmask
    uint32_t rip;                 // our default router IP address
    uint16_t mtu;                 // our MTU
    uint8_t mac[ETH_ADDR_LEN];    // our Ethernet address
    int fd;                       // netmap descriptor
    void * mem;                   // netmap memory
    struct nmreq req;             // netmap request
    uint8_t unused[4];
    SLIST_ENTRY(warpcore) next; // next engine
};


#define mk_bcast(ip, mask) ((ip) | (~mask))

#define mk_net(ip, mask) ((ip) & (mask))

#define get_sock(w, p, port) ((p) == IP_P_UDP ? &(w)->udp[port] : 0)
