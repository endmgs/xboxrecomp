/**
 * The network card's cable: Ethernet frames over UDP.
 *
 * See xbox_net_udp.c for the formats and the switches.
 */
#ifndef XBOX_NET_UDP_H
#define XBOX_NET_UDP_H

#include <stdint.h>

/* The largest frame carried: a full Ethernet frame without its checksum. */
#define XBOX_NET_FRAME_MAX 1514u

/* The port a host listens on when nobody chooses one. */
#define XBOX_NET_DEFAULT_PORT 27610

/* Consoles in one hosted session, the host's own included. */
#define XBOX_NET_MAX_CONSOLES 16

/* Open the socket and start receiving, as the settings say (RECOMP_SYSLINK
 * / syslink and the rest; see xbox_net_udp.c). Returns 1 when the tunnel is
 * up. own_mac is this console's address, so frames that come back to it are
 * not delivered. */
int  xbox_NetUdpStart(const uint8_t own_mac[6]);

/* Send one frame now. */
void xbox_NetUdpSend(const uint8_t *frame, uint32_t len);

/* The next received frame that is due (RECOMP_SYSLINK_DELAY_MS holds frames
 * back), copied to out without taking it: its length, or 0 when none is due.
 * xbox_NetUdpPop then takes it -- the card pops only once a receive buffer
 * has accepted the frame, so a full ring delays a frame instead of losing
 * it. */
uint32_t xbox_NetUdpPeek(uint8_t *out, uint32_t cap);
void     xbox_NetUdpPop(void);

/* Counters for the summary line. */
void xbox_NetUdpStats(unsigned long *sent, unsigned long *received,
                      unsigned long *dropped);

/* ---- underneath, for tests ------------------------------------------- */

/* How a link is joined to the others. */
typedef enum XboxNetMode {
    XBOX_NET_OFF = 0,
    XBOX_NET_PAIR,      /* "udp": bare frames to one address, xemu's format   */
    XBOX_NET_HOST,      /* "host": the switch every joined console goes through */
    XBOX_NET_JOIN,      /* "join": one console, through a host               */
    XBOX_NET_NAT        /* "nat": the internet, through a router in the
                         * runtime (xbox_net_nat.c); xbox_NetUdpStart opens
                         * that instead of a link */
} XboxNetMode;

typedef struct XboxNetConfig {
    XboxNetMode mode;
    char  remote[256];      /* PAIR: where frames go; JOIN: the host, host[:port] */
    int   port;             /* HOST: where to listen; PAIR: where to receive (0 any);
                             * JOIN: the host's port when `remote` names none */
    char  code[64];         /* HOST/JOIN: the session's join code, "" for none */
    int   loopback_only;    /* bind 127.0.0.1, so a test asks no firewall anything */
    int   quiet;            /* no log lines */
    long  delay_ms;         /* added to every frame delivered here */
    int   loss_pct;         /* percentage of delivered frames dropped */
} XboxNetConfig;

typedef struct XboxNetLink XboxNetLink;

/* A link of its own: what xbox_NetUdpStart opens once, and what a test
 * opens several of, in one process, to stand in for several consoles. */
XboxNetLink *xbox_net_link_open(const XboxNetConfig *cfg, const uint8_t own_mac[6]);
void         xbox_net_link_close(XboxNetLink *l);
void         xbox_net_link_send(XboxNetLink *l, const uint8_t *frame, uint32_t len);
uint32_t     xbox_net_link_peek(XboxNetLink *l, uint8_t *out, uint32_t cap);
void         xbox_net_link_pop(XboxNetLink *l);
int          xbox_net_link_port(const XboxNetLink *l);       /* the port bound */
int          xbox_net_link_joined(const XboxNetLink *l);     /* JOIN: welcomed */
int          xbox_net_link_consoles(const XboxNetLink *l);   /* in the session */

/* Where the switch sends a frame. `from` is the peer it came from, -1 for the
 * host's own card. Peers are listed by their learned Ethernet address,
 * has_mac[i] 0 for one not learned yet. Sets *to_local when the host's own
 * card takes it, fills `to` with peer indices, and returns how many. */
int xbox_net_route(const uint8_t dest[6], const uint8_t own_mac[6],
                   const uint8_t (*peer_mac)[6], const int *has_mac, int peers,
                   int from, int *to_local, int *to);

#endif /* XBOX_NET_UDP_H */
