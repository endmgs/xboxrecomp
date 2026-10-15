/**
 * The network card's way to the internet: a small router inside the runtime.
 *
 * See xbox_net_nat.c for what it answers and what it carries.
 */
#ifndef XBOX_NET_NAT_H
#define XBOX_NET_NAT_H

#include <stdint.h>

/* The network behind the router, as QEMU's user networking (and so xemu's
 * NAT mode) lays it out: the console is given .15, the router is .2. Host
 * byte order. */
#define XBOX_NAT_NET      0x0A000200u    /* 10.0.2.0/24 */
#define XBOX_NAT_MASK     0xFFFFFF00u
#define XBOX_NAT_GATEWAY  0x0A000202u    /* 10.0.2.2  */
#define XBOX_NAT_CONSOLE  0x0A00020Fu    /* 10.0.2.15 */

/* The DNS servers handed out when nobody chooses: Insignia's first, as its
 * connection guide asks, then a public resolver for everything else. */
#define XBOX_NAT_DEFAULT_DNS "46.101.64.175,1.1.1.1"

typedef struct XboxNatConfig {
    char dns[128];          /* comma-separated IPv4 addresses, "" for the default */
    int  fresh_ports;       /* 1: never try the console's own port on this PC */
    int  quiet;             /* no log lines */
} XboxNatConfig;

typedef struct XboxNat XboxNat;

/* Start the router for the console whose Ethernet address is own_mac. NULL
 * if it cannot (no socket, no thread). */
XboxNat *xbox_net_nat_open(const XboxNatConfig *cfg, const uint8_t own_mac[6]);
void     xbox_net_nat_close(XboxNat *n);

/* A frame from the card. Safe in the card's fault handler: it only queues
 * the frame and wakes the router's thread. */
void     xbox_net_nat_send(XboxNat *n, const uint8_t *frame, uint32_t len);

/* Frames for the card, as xbox_net_link_peek / _pop (xbox_net_udp.h). */
uint32_t xbox_net_nat_peek(XboxNat *n, uint8_t *out, uint32_t cap);
void     xbox_net_nat_pop(XboxNat *n);

/* Frames from the card, frames to it, and frames dropped either way. */
void     xbox_net_nat_stats(XboxNat *n, unsigned long *sent,
                            unsigned long *received, unsigned long *dropped);

/* Whether the console has taken its address (a DHCP ACK was sent). */
int      xbox_net_nat_leased(const XboxNat *n);

#endif /* XBOX_NET_NAT_H */
