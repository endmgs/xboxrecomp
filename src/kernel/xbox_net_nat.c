/**
 * The network card's way to the internet: a small router inside the runtime.
 *
 * System Link needs only a cable between consoles (xbox_net_udp.c). Xbox
 * Live -- today Insignia -- needs the internet, and the console's network
 * stack (XNet, linked into the title) expects what a home router gives it:
 * an address by DHCP, a gateway that answers ARP, and DNS. This is that
 * router, as QEMU's user networking is for xemu, kept to what XNet uses:
 *
 *     syslink = nat                                  [RECOMP_SYSLINK]
 *     net_dns = 46.101.64.175,1.1.1.1                [RECOMP_NET_DNS]
 *     RECOMP_NET_FRESH_PORTS=1   never try the console's own port on this PC
 *     RECOMP_NET_LOG=1           a line for each new UDP mapping and remote
 *     RECOMP_NET_PCAP=<file>     every frame between the card and the router,
 *                                both ways, as a pcap Wireshark opens. It
 *                                holds the console's sign-in traffic: keep it
 *                                to yourself.
 *
 * The network behind it is 10.0.2.0/24 (xbox_net_nat.h): the console is
 * given 10.0.2.15, the router is 10.0.2.2, and the DNS servers are handed
 * out as they are, Insignia's first, so the console asks Insignia itself
 * and nothing here has to know a name.
 *
 * What it carries:
 *
 *   ARP      who-has for the router is answered; nothing else is (XNet
 *            probes its own new address and must hear nothing).
 *   DHCP     DISCOVER, REQUEST and INFORM, answered at once. On a console
 *            with nothing answering, XNet retries for half a minute or more.
 *   UDP      to anywhere outside the network, through one socket on this
 *            PC per port the console sends from. That socket is bound to
 *            the console's own port when it is free (Xbox Live's 3074 then
 *            leaves this PC as 3074), and answers from anyone come back to
 *            the console from where they came, as an endpoint-independent
 *            ("full cone") NAT does. A mapping idle for five minutes closes.
 *   IP fragments, both ways: the console's are put back together before
 *            they go out, and a datagram for it too big for one frame is
 *            split (a Kerberos ticket can be).
 *   ICMP     echo to the router is answered. Nothing else is carried.
 *   TCP      relayed: the router ends the console's connection itself and
 *            carries its bytes over a socket of this PC's to the same address
 *            and port (the dashboard's online updater downloads over HTTP).
 *            The console's SYN is answered once this PC's connect succeeds,
 *            or reset when it fails. TCP_MAX connections at once, each with
 *            TCP_BUF bytes buffered either way; data the console has not
 *            acknowledged is sent again after TCP_RTO_MS.
 *
 * Threads. A frame from the card arrives in the card's fault handler -- a
 * signal handler on POSIX -- so xbox_net_nat_send only copies it into a
 * queue and wakes the router with a byte to a loopback socket of its own.
 * The router's thread does everything else, and frames for the card wait in
 * a second queue the kernel's timer thread drains (xbox_NicTick).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_net_nat.h"
#include "xbox_net_udp.h"   /* XBOX_NET_FRAME_MAX */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET net_socket;
#define NET_BAD_SOCKET INVALID_SOCKET
#define net_close closesocket
#define NET_WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#define NET_SEND_FLAGS 0
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
typedef int net_socket;
#define NET_BAD_SOCKET (-1)
#define net_close close
#define NET_WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)
#ifdef MSG_NOSIGNAL
#define NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define NET_SEND_FLAGS 0
#endif
#endif

#define OUT_LEN      64          /* frames from the card, not yet handled   */
#define IN_LEN       256         /* frames for the card                     */
#define UDP_MAX      32          /* console ports carried at once           */
#define UDP_IDLE_MS  300000u
#define FRAG_SLOTS   4
#define FRAG_MAX     16384u
#define FRAG_IDLE_MS 5000u
#define IP_PAYLOAD   1480u       /* in one 1514-byte frame                  */
#define LEASE_S      86400u
#define DNS_LINES    48          /* names asked for, said; then quiet       */
#define TCP_MAX      8           /* TCP connections carried at once         */
#define TCP_BUF      65536u      /* bytes buffered each way, per connection */
#define TCP_RTO_MS   400u        /* unacknowledged data is sent again       */
#define TCP_IDLE_MS  300000u

#define ETH_IPV4 0x0800u
#define ETH_ARP  0x0806u
#define IP_ICMP  1u
#define IP_TCP   6u
#define IP_UDP   17u

/* The router's own Ethernet address: QEMU's, which is what xemu's console
 * sees behind its NAT too. */
static const uint8_t k_router_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
static const uint8_t k_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

typedef struct NatUdp {
    int                used;
    uint16_t           console_port;
    uint32_t           console_ip;      /* as it last sent from */
    net_socket         sock;
    uint16_t           host_port;       /* bound on this PC */
    unsigned long long last;
} NatUdp;

/* A console TCP connection, relayed over a socket of this PC's. Sequence
 * numbers toward the console are ours (iss, una, nxt); from it, rcv_nxt.
 * tx holds what the PC's socket gave, from sequence tx_seq, until the
 * console acknowledges it; rx holds what the console sent, until the PC's
 * socket takes it. */
typedef struct NatTcp {
    int                used;
    int                connecting;      /* this PC's connect not done yet */
    net_socket         sock;
    uint32_t           console_ip, remote_ip;
    uint16_t           console_port, remote_port;
    uint32_t           iss, una, nxt, rcv_nxt;
    int                syn_acked;
    uint32_t           wnd;             /* the console's window */
    uint32_t           mss;             /* the console's, or 536 */
    uint8_t           *tx;
    uint32_t           tx_seq, tx_len;
    uint8_t           *rx;
    uint32_t           rx_len;
    int                host_eof;        /* this PC's side has finished */
    int                fin_sent, fin_acked;
    uint32_t           fin_seq;
    int                console_fin;     /* the console has finished */
    int                shut_wr;         /* passed on to this PC's socket */
    unsigned long long last, progress;
    unsigned long      bytes_in, bytes_out;
} NatTcp;

typedef struct Frag {
    int                used;
    uint32_t           src, dst;
    uint16_t           id;
    uint8_t            proto;
    uint32_t           have, total;     /* total 0 until the last piece */
    unsigned long long t;
    uint8_t            data[FRAG_MAX];
} Frag;

typedef struct Slot {
    uint32_t len;
    uint8_t  data[XBOX_NET_FRAME_MAX];
} Slot;

struct XboxNat {
    XboxNatConfig      cfg;
    uint8_t            console_mac[6];
    uint32_t           dns[4];
    int                ndns;
    int                verbose;
    FILE              *pcap;            /* RECOMP_NET_PCAP; the router thread's */

    net_socket         wake;            /* bound to 127.0.0.1, sent to itself */
    struct sockaddr_in wake_to;

    Slot               out[OUT_LEN];
    unsigned           out_head, out_count;
    Slot               in[IN_LEN];
    unsigned           in_head, in_count;

    NatUdp             udp[UDP_MAX];
    Frag               frag[FRAG_SLOTS];
    uint16_t           ip_id;
    int                leased;
    long               dns_lines;
    NatTcp             tcp[TCP_MAX];
    unsigned long      sent, received, dropped;

    uint8_t            scratch[65536 + 8];   /* the router thread's own */

    volatile int       stop;
#ifdef _WIN32
    CRITICAL_SECTION   lock;
    HANDLE             thread;
#else
    pthread_mutex_t    lock;
    pthread_t          thread;
    int                have_thread;
#endif
};

/* ---- small things ------------------------------------------------------ */

static void lock(XboxNat *n)
{
#ifdef _WIN32
    EnterCriticalSection(&n->lock);
#else
    pthread_mutex_lock(&n->lock);
#endif
}

static void unlock(XboxNat *n)
{
#ifdef _WIN32
    LeaveCriticalSection(&n->lock);
#else
    pthread_mutex_unlock(&n->lock);
#endif
}

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u;
#endif
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void say(const XboxNat *n, const char *fmt, ...)
{
    va_list ap;

    if (n->cfg.quiet)
        return;
    va_start(ap, fmt);
    fputs("[NET] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* pcap (libpcap's own format, microsecond stamps, Ethernet frames). Written
 * on the router's thread only: every frame either way passes through it. */
static void pcap_open(XboxNat *n, const char *path)
{
    static const uint8_t hdr[24] = {
        0xD4, 0xC3, 0xB2, 0xA1, 2, 0, 4, 0,   /* magic, version 2.4 */
        0, 0, 0, 0, 0, 0, 0, 0,               /* zone, accuracy */
        0xFF, 0xFF, 0, 0, 1, 0, 0, 0          /* snaplen 65535, Ethernet */
    };
    n->pcap = fopen(path, "wb");
    if (!n->pcap) {
        say(n, "RECOMP_NET_PCAP: cannot write %s", path);
        return;
    }
    fwrite(hdr, 1, sizeof hdr, n->pcap);
    fflush(n->pcap);
    say(n, "writing the console's frames to %s", path);
}

static void pcap_frame(XboxNat *n, const uint8_t *f, uint32_t len)
{
    uint8_t rec[16];
    uint32_t sec, usec;
    int i;
#ifdef _WIN32
    FILETIME ft;
    unsigned long long t;
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10u
        - 11644473600000000ull;
    sec = (uint32_t)(t / 1000000u);
    usec = (uint32_t)(t % 1000000u);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec = (uint32_t)ts.tv_sec;
    usec = (uint32_t)(ts.tv_nsec / 1000);
#endif
    if (!n->pcap)
        return;
    for (i = 0; i < 4; i++) {
        rec[i] = (uint8_t)(sec >> (8 * i));
        rec[4 + i] = (uint8_t)(usec >> (8 * i));
        rec[8 + i] = rec[12 + i] = (uint8_t)(len >> (8 * i));
    }
    fwrite(rec, 1, sizeof rec, n->pcap);
    fwrite(f, 1, len, n->pcap);
    fflush(n->pcap);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static const char *ip_text(uint32_t ip, char *buf, size_t n)
{
    snprintf(buf, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
    return buf;
}

/* The Internet checksum: a ones'-complement sum of 16-bit words. */
static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t s)
{
    while (len > 1) {
        s += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        s += (uint32_t)p[0] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}

/* The sum of the pseudo-header UDP and TCP checksums start from. */
static uint32_t pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len)
{
    return (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + proto + len;
}

static int in_network(uint32_t ip)
{
    return (ip & XBOX_NAT_MASK) == XBOX_NAT_NET;
}

/* ---- frames for the card ----------------------------------------------- */

static void deliver(XboxNat *n, const uint8_t *frame, uint32_t len)
{
    pcap_frame(n, frame, len);
    lock(n);
    if (n->in_count == IN_LEN) {
        n->dropped++;
    } else {
        Slot *s = &n->in[(n->in_head + n->in_count) % IN_LEN];
        memcpy(s->data, frame, len);
        s->len = len;
        n->in_count++;
        n->received++;
    }
    unlock(n);
}

/* One IP datagram from `src` to the console, split into fragments when it
 * does not fit a frame. */
static void deliver_ip(XboxNat *n, const uint8_t dst_mac[6], uint8_t proto,
                       uint32_t src, uint32_t dst, const uint8_t *payload,
                       uint32_t plen)
{
    uint8_t f[XBOX_NET_FRAME_MAX];
    uint16_t id = n->ip_id++;
    uint32_t off = 0;

    do {
        uint32_t chunk = plen - off;
        int more = 0;
        uint8_t *ip = f + 14;

        if (chunk > IP_PAYLOAD) {
            chunk = IP_PAYLOAD & ~7u;
            more = 1;
        }
        memcpy(f, dst_mac, 6);
        memcpy(f + 6, k_router_mac, 6);
        put16(f + 12, ETH_IPV4);
        ip[0] = 0x45;
        ip[1] = 0;
        put16(ip + 2, 20u + chunk);
        put16(ip + 4, id);
        put16(ip + 6, (more ? 0x2000u : 0u) | (off / 8u));
        ip[8] = 64;
        ip[9] = proto;
        put16(ip + 10, 0);
        put32(ip + 12, src);
        put32(ip + 16, dst);
        put16(ip + 10, fold(sum16(ip, 20, 0)));
        memcpy(ip + 20, payload + off, chunk);
        deliver(n, f, 14u + 20u + chunk);
        off += chunk;
    } while (off < plen);
}

static void deliver_udp(XboxNat *n, const uint8_t dst_mac[6],
                        uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                        const uint8_t *data, uint32_t len)
{
    uint8_t *u = n->scratch;
    uint32_t ulen = 8u + len;
    uint16_t sum;

    if (ulen > sizeof n->scratch)
        return;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, ulen);
    put16(u + 6, 0);
    memcpy(u + 8, data, len);
    sum = fold(sum16(u, ulen, pseudo(src, dst, IP_UDP, ulen)));
    put16(u + 6, sum ? sum : 0xFFFFu);
    deliver_ip(n, dst_mac, IP_UDP, src, dst, u, ulen);
}

/* ---- ARP --------------------------------------------------------------- */

static void arp_in(XboxNat *n, const uint8_t *a, uint32_t len)
{
    uint8_t f[42];
    uint32_t spa, tpa;

    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IPV4 || a[4] != 6 ||
        a[5] != 4 || get16(a + 6) != 1)
        return;
    spa = get32(a + 14);
    tpa = get32(a + 24);
    if (tpa != XBOX_NAT_GATEWAY || spa == tpa)
        return;
    memcpy(f, a + 8, 6);
    memcpy(f + 6, k_router_mac, 6);
    put16(f + 12, ETH_ARP);
    put16(f + 14, 1);
    put16(f + 16, ETH_IPV4);
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, 2);                     /* is-at */
    memcpy(f + 22, k_router_mac, 6);
    put32(f + 28, tpa);
    memcpy(f + 32, a + 8, 6);
    put32(f + 38, spa);
    deliver(n, f, sizeof f);
}

/* ---- DHCP -------------------------------------------------------------- */

enum { DHCP_DISCOVER = 1, DHCP_OFFER, DHCP_REQUEST, DHCP_DECLINE, DHCP_ACK,
       DHCP_NAK, DHCP_RELEASE, DHCP_INFORM };

static uint8_t *opt4(uint8_t *o, uint8_t code, uint32_t v)
{
    o[0] = code;
    o[1] = 4;
    put32(o + 2, v);
    return o + 6;
}

static void dhcp_in(XboxNat *n, const uint8_t *b, uint32_t len)
{
    uint8_t r[548], *o;
    uint32_t i, want = 0, ciaddr;
    int type = 0, reply, bcast;
    char ip[16], d0[16], d1[16];

    if (len < 240 || b[0] != 1 || b[1] != 1 || b[2] != 6 || get32(b + 236) != 0x63825363u)
        return;
    for (i = 240; i < len; ) {
        uint8_t code = b[i], l;
        if (code == 0) { i++; continue; }
        if (code == 255 || i + 1 >= len)
            break;
        l = b[i + 1];
        if (i + 2u + l > len)
            break;
        if (code == 53 && l >= 1)
            type = b[i + 2];
        else if (code == 50 && l == 4)
            want = get32(b + i + 2);
        i += 2u + l;
    }
    ciaddr = get32(b + 12);
    switch (type) {
    case DHCP_DISCOVER: reply = DHCP_OFFER; break;
    case DHCP_REQUEST:
        if (!want)
            want = ciaddr;
        reply = (!want || want == XBOX_NAT_CONSOLE) ? DHCP_ACK : DHCP_NAK;
        break;
    case DHCP_INFORM:   reply = DHCP_ACK; break;
    default:            return;          /* RELEASE, DECLINE: nothing to say */
    }

    memset(r, 0, sizeof r);
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, b + 4, 4);              /* xid */
    memcpy(r + 10, b + 10, 2);            /* flags */
    if (type == DHCP_INFORM)
        memcpy(r + 12, b + 12, 4);
    else if (reply != DHCP_NAK)
        put32(r + 16, XBOX_NAT_CONSOLE);
    memcpy(r + 28, b + 28, 16);           /* chaddr */
    put32(r + 236, 0x63825363u);
    o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)reply;
    o = opt4(o, 54, XBOX_NAT_GATEWAY);
    if (reply != DHCP_NAK) {
        if (type != DHCP_INFORM) {
            o = opt4(o, 51, LEASE_S);
            o = opt4(o, 58, LEASE_S / 2u);
            o = opt4(o, 59, LEASE_S / 8u * 7u);
        }
        o = opt4(o, 1, XBOX_NAT_MASK);
        o = opt4(o, 3, XBOX_NAT_GATEWAY);
        o = opt4(o, 28, XBOX_NAT_NET | ~XBOX_NAT_MASK);
        if (n->ndns) {
            int k;
            *o++ = 6;
            *o++ = (uint8_t)(4 * n->ndns);
            for (k = 0; k < n->ndns; k++, o += 4)
                put32(o, n->dns[k]);
        }
    }
    *o++ = 255;

    /* RFC 2131 4.1: broadcast when the client asks for it (it cannot take a
     * unicast before it has an address) or for a NAK; otherwise to the
     * address it is being given, at its own Ethernet address. */
    bcast = (get16(b + 10) & 0x8000u) || reply == DHCP_NAK || type == DHCP_INFORM;
    deliver_udp(n, bcast ? k_broadcast : b + 28, XBOX_NAT_GATEWAY, 67,
                type == DHCP_INFORM && ciaddr ? ciaddr
                    : bcast ? 0xFFFFFFFFu : XBOX_NAT_CONSOLE, 68,
                r, 300u > (uint32_t)(o - r) ? 300u : (uint32_t)(o - r));

    if (reply == DHCP_ACK && type == DHCP_REQUEST && !n->leased) {
        n->leased = 1;
        say(n, "internet through this PC: the console is %s behind the "
               "router at 10.0.2.2, DNS %s%s%s",
            ip_text(XBOX_NAT_CONSOLE, ip, sizeof ip),
            n->ndns ? ip_text(n->dns[0], d0, sizeof d0) : "none",
            n->ndns > 1 ? " then " : "",
            n->ndns > 1 ? ip_text(n->dns[1], d1, sizeof d1) : "");
    } else if (reply == DHCP_NAK) {
        say(n, "DHCP: the console asked for %s, refused (it is given %s)",
            ip_text(want, ip, sizeof ip), ip_text(XBOX_NAT_CONSOLE, d0, sizeof d0));
    }
}

/* ---- UDP --------------------------------------------------------------- */

static void udp_close(NatUdp *m)
{
    if (m->used && m->sock != NET_BAD_SOCKET)
        net_close(m->sock);
    m->used = 0;
    m->sock = NET_BAD_SOCKET;
}

static int bind_port(net_socket s, uint16_t port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    return bind(s, (const struct sockaddr *)&a, (int)sizeof a) == 0;
}

/* The socket that carries what the console sends from console_port. */
static NatUdp *udp_for(XboxNat *n, uint32_t console_ip, uint16_t console_port)
{
    NatUdp *m = NULL, *oldest = NULL;
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int i;

    for (i = 0; i < UDP_MAX; i++) {
        NatUdp *u = &n->udp[i];
        if (u->used && u->console_port == console_port) {
            u->console_ip = console_ip;
            u->last = now_ms();
            return u;
        }
        if (!u->used && !m)
            m = u;
        if (u->used && (!oldest || u->last < oldest->last))
            oldest = u;
    }
    if (!m) {
        udp_close(oldest);
        m = oldest;
    }

    m->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m->sock == NET_BAD_SOCKET)
        return NULL;
#ifdef _WIN32
    {
        BOOL report = FALSE;
        DWORD got;
        WSAIoctl(m->sock, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0,
                 &got, NULL, NULL);
    }
#endif
    if ((n->cfg.fresh_ports || !bind_port(m->sock, console_port)) &&
        !bind_port(m->sock, 0)) {
        net_close(m->sock);
        m->sock = NET_BAD_SOCKET;
        return NULL;
    }
    memset(&a, 0, sizeof a);
    getsockname(m->sock, (struct sockaddr *)&a, &al);
    m->used = 1;
    m->console_port = console_port;
    m->console_ip = console_ip;
    m->host_port = ntohs(a.sin_port);
    m->last = now_ms();
    if (n->verbose || (console_port != m->host_port && !n->cfg.fresh_ports))
        say(n, "UDP from the console's port %u leaves this PC from port %u%s",
            console_port, m->host_port,
            console_port != m->host_port ? " (its own was taken)" : "");
    return m;
}

/* The name a DNS query asks for, said while the budget lasts: the clearest
 * view of what a title is trying to reach. */
static void say_dns(XboxNat *n, const uint8_t *d, uint32_t len, uint32_t server)
{
    char name[256], s[16];
    uint32_t i = 12, o = 0;

    if (n->dns_lines >= DNS_LINES || len < 17 || (d[2] & 0x80) || get16(d + 4) == 0)
        return;
    while (i < len && d[i] && o < sizeof name - 2) {
        uint32_t l = d[i++], k;
        if (l > 63 || i + l > len)
            return;
        if (o)
            name[o++] = '.';
        for (k = 0; k < l && o < sizeof name - 1; k++) {
            uint8_t c = d[i + k];
            name[o++] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
        }
        i += l;
    }
    name[o] = 0;
    n->dns_lines++;
    say(n, "DNS: %s, asked of %s%s", o ? name : "(root)", ip_text(server, s, sizeof s),
        n->dns_lines == DNS_LINES ? " (no more DNS lines)" : "");
}

static void udp_in(XboxNat *n, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t plen)
{
    uint16_t sport, dport, ulen;
    struct sockaddr_in to;
    NatUdp *m;

    if (plen < 8)
        return;
    sport = get16(p);
    dport = get16(p + 2);
    ulen = get16(p + 4);
    if (ulen < 8 || ulen > plen)
        return;
    if (dport == 67 && (dst == 0xFFFFFFFFu || dst == XBOX_NAT_GATEWAY)) {
        dhcp_in(n, p + 8, ulen - 8u);
        return;
    }
    /* Broadcasts (System Link's "who is hosting"), multicast, and anything
     * else on this network: there is nobody behind the router to hear it. */
    if (dst == 0xFFFFFFFFu || (dst >> 28) == 14u || in_network(dst))
        return;
    if (dport == 53)
        say_dns(n, p + 8, ulen - 8u, dst);

    m = udp_for(n, src, sport);
    if (!m) {
        n->dropped++;
        return;
    }
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(dst);
    to.sin_port = htons(dport);
    if (sendto(m->sock, (const char *)p + 8, (int)(ulen - 8u), 0,
               (const struct sockaddr *)&to, (int)sizeof to) < 0)
        n->dropped++;
    m->last = now_ms();
}

/* ---- TCP and ICMP ------------------------------------------------------ */

/* Sequence comparisons, modulo 2^32. */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static void tcp_segment(XboxNat *n, NatTcp *c, uint32_t seq, uint8_t flags,
                        const uint8_t *data, uint32_t len)
{
    uint8_t *t = n->scratch;
    uint32_t hl = (flags & 0x02) ? 24u : 20u;   /* an MSS option on a SYN */
    uint32_t room = TCP_BUF - c->rx_len;

    memset(t, 0, hl);
    put16(t, c->remote_port);
    put16(t + 2, c->console_port);
    put32(t + 4, seq);
    put32(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)((hl / 4u) << 4);
    t[13] = flags;
    put16(t + 14, room > 65535u ? 65535u : room);
    if (hl == 24u) {
        t[20] = 2; t[21] = 4;
        put16(t + 22, IP_PAYLOAD - 20u);
    }
    if (len)
        memcpy(t + hl, data, len);
    put16(t + 16, fold(sum16(t, hl + len, pseudo(c->remote_ip, c->console_ip, IP_TCP, hl + len))));
    deliver_ip(n, n->console_mac, IP_TCP, c->remote_ip, c->console_ip, t, hl + len);
}

/* A reset for a segment that belongs to no connection, as RFC 793 says. */
static void tcp_reset_for(XboxNat *n, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t len)
{
    uint8_t t[20];
    uint8_t fl = p[13];
    uint32_t dlen = len - (uint32_t)(p[12] >> 4) * 4u;

    if (fl & 0x04)
        return;
    memset(t, 0, sizeof t);
    put16(t, get16(p + 2));
    put16(t + 2, get16(p));
    if (fl & 0x10) {
        put32(t + 4, get32(p + 8));
        t[13] = 0x04;
    } else {
        put32(t + 8, get32(p + 4) + dlen + ((fl & 0x02) ? 1u : 0u) + ((fl & 0x01) ? 1u : 0u));
        t[13] = 0x14;
    }
    t[12] = 5u << 4;
    put16(t + 16, fold(sum16(t, 20, pseudo(dst, src, IP_TCP, 20))));
    deliver_ip(n, n->console_mac, IP_TCP, dst, src, t, sizeof t);
}

static void tcp_close(XboxNat *n, NatTcp *c, const char *why)
{
    char s[16];

    if (!c->used)
        return;
    if (why && n->verbose)
        say(n, "TCP %s:%u (console port %u): %s, %lu bytes out, %lu in",
            ip_text(c->remote_ip, s, sizeof s), c->remote_port, c->console_port, why,
            c->bytes_out, c->bytes_in);
    if (c->sock != NET_BAD_SOCKET)
        net_close(c->sock);
    free(c->tx);
    free(c->rx);
    memset(c, 0, sizeof *c);
    c->sock = NET_BAD_SOCKET;
}

static void tcp_abort(XboxNat *n, NatTcp *c, const char *why)
{
    if (c->syn_acked || !c->connecting)
        tcp_segment(n, c, c->nxt, 0x14, NULL, 0);   /* RST, ACK */
    tcp_close(n, c, why);
}

/* Send what the console's window allows: the SYN-ACK until it is
 * acknowledged, then data, then a FIN once this PC's side has finished. */
static void tcp_pump(XboxNat *n, NatTcp *c)
{
    if (c->connecting)
        return;
    if (!c->syn_acked) {
        if (c->nxt == c->iss) {
            tcp_segment(n, c, c->iss, 0x12, NULL, 0);   /* SYN, ACK */
            c->nxt = c->iss + 1u;
        }
        return;
    }
    for (;;) {
        uint32_t off = c->nxt - c->tx_seq;
        uint32_t inflight = c->nxt - c->una;
        uint32_t len;

        if (off >= c->tx_len || inflight >= c->wnd)
            break;
        len = c->tx_len - off;
        if (len > c->mss)
            len = c->mss;
        if (len > c->wnd - inflight)
            len = c->wnd - inflight;
        tcp_segment(n, c, c->nxt, 0x18, c->tx + off, len);   /* PSH, ACK */
        c->nxt += len;
    }
    if (c->host_eof && !c->fin_sent && c->nxt == c->tx_seq + c->tx_len) {
        c->fin_seq = c->nxt;
        tcp_segment(n, c, c->nxt, 0x11, NULL, 0);           /* FIN, ACK */
        c->nxt++;
        c->fin_sent = 1;
    }
}

/* What the console sent, to this PC's socket. */
static void tcp_flush_rx(XboxNat *n, NatTcp *c)
{
    while (c->rx_len && !c->connecting) {
        int r = (int)send(c->sock, (const char *)c->rx, (int)c->rx_len, NET_SEND_FLAGS);
        if (r <= 0) {
            if (r < 0 && NET_WOULD_BLOCK())
                return;
            tcp_abort(n, c, "this PC's socket refused the console's data");
            return;
        }
        memmove(c->rx, c->rx + r, c->rx_len - (uint32_t)r);
        c->rx_len -= (uint32_t)r;
        c->bytes_out += (unsigned long)r;
        c->last = now_ms();
        /* The window it opens is worth saying. */
        tcp_segment(n, c, c->nxt, 0x10, NULL, 0);
    }
    if (!c->rx_len && c->console_fin && !c->shut_wr) {
        shutdown(c->sock, 1);                               /* SHUT_WR / SD_SEND */
        c->shut_wr = 1;
    }
}

static NatTcp *tcp_find(XboxNat *n, uint32_t remote, uint16_t rport, uint16_t cport)
{
    int i;
    for (i = 0; i < TCP_MAX; i++)
        if (n->tcp[i].used && n->tcp[i].remote_ip == remote &&
            n->tcp[i].remote_port == rport && n->tcp[i].console_port == cport)
            return &n->tcp[i];
    return NULL;
}

static void tcp_open(XboxNat *n, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t len)
{
    NatTcp *c = NULL;
    struct sockaddr_in a;
    uint32_t hl = (uint32_t)(p[12] >> 4) * 4u, i;
    char s[16];
    int r;

    for (i = 0; i < TCP_MAX; i++)
        if (!n->tcp[i].used) {
            c = &n->tcp[i];
            break;
        }
    if (!c) {
        say(n, "TCP to %s port %u: %d connections already carried, refused",
            ip_text(dst, s, sizeof s), get16(p + 2), TCP_MAX);
        tcp_reset_for(n, src, dst, p, len);
        return;
    }
    memset(c, 0, sizeof *c);
    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    c->tx = (uint8_t *)malloc(TCP_BUF);
    c->rx = (uint8_t *)malloc(TCP_BUF);
    if (c->sock == NET_BAD_SOCKET || !c->tx || !c->rx) {
        c->used = 1;
        tcp_close(n, c, NULL);
        tcp_reset_for(n, src, dst, p, len);
        return;
    }
#ifdef _WIN32
    {
        u_long nb = 1;
        ioctlsocket(c->sock, FIONBIO, &nb);
    }
#else
    fcntl(c->sock, F_SETFL, fcntl(c->sock, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(c->sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
#endif
    c->used = 1;
    c->connecting = 1;
    c->console_ip = src;
    c->console_port = get16(p);
    c->remote_ip = dst;
    c->remote_port = get16(p + 2);
    c->rcv_nxt = get32(p + 4) + 1u;
    c->iss = (uint32_t)(now_ms() * 2654435761u) ^ ((uint32_t)c->console_port << 16);
    c->una = c->nxt = c->iss;
    c->tx_seq = c->iss + 1u;
    c->wnd = get16(p + 14);
    c->mss = 536u;
    for (i = 20; i + 1 < hl && i + 1 < len; ) {           /* the MSS option */
        uint8_t kind = p[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (p[i + 1] < 2)
            break;
        if (kind == 2 && p[i + 1] == 4 && i + 3 < hl)
            c->mss = get16(p + i + 2);
        i += p[i + 1];
    }
    if (c->mss > IP_PAYLOAD - 20u)
        c->mss = IP_PAYLOAD - 20u;
    c->last = c->progress = now_ms();

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(dst);
    a.sin_port = htons(c->remote_port);
    r = connect(c->sock, (const struct sockaddr *)&a, (int)sizeof a);
    if (r != 0 && !NET_WOULD_BLOCK()) {
        say(n, "TCP to %s port %u: this PC cannot connect, refused",
            ip_text(dst, s, sizeof s), c->remote_port);
        tcp_close(n, c, NULL);
        tcp_reset_for(n, src, dst, p, len);
        return;
    }
    if (n->verbose)
        say(n, "TCP from the console's port %u to %s:%u: connecting from this PC",
            c->console_port, ip_text(dst, s, sizeof s), c->remote_port);
}

/* A segment from the console. */
static void tcp_in(XboxNat *n, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t plen)
{
    NatTcp *c;
    uint32_t hl, seq, ack, dlen;
    uint8_t fl;

    if (plen < 20)
        return;
    hl = (uint32_t)(p[12] >> 4) * 4u;
    if (hl < 20 || hl > plen)
        return;
    fl = p[13];
    seq = get32(p + 4);
    ack = get32(p + 8);
    dlen = plen - hl;
    c = tcp_find(n, dst, get16(p + 2), get16(p));
    if (!c) {
        if ((fl & 0x02) && !(fl & 0x10) && !(fl & 0x04))   /* a new SYN */
            tcp_open(n, src, dst, p, plen);
        else
            tcp_reset_for(n, src, dst, p, plen);
        return;
    }
    c->last = now_ms();
    if (fl & 0x04) {
        tcp_close(n, c, "reset by the console");
        return;
    }
    if (fl & 0x02) {                                       /* its SYN again */
        if (!c->connecting && !c->syn_acked) {
            c->nxt = c->iss;
            tcp_pump(n, c);
        }
        return;
    }
    if (c->connecting)
        return;
    if (fl & 0x10) {
        if (seq_lt(c->una, ack) && seq_le(ack, c->nxt)) {
            if (!c->syn_acked && seq_le(c->iss + 1u, ack))
                c->syn_acked = 1;
            if (seq_lt(c->tx_seq, ack)) {
                uint32_t done = ack - c->tx_seq;
                if (done > c->tx_len)
                    done = c->tx_len;
                memmove(c->tx, c->tx + done, c->tx_len - done);
                c->tx_len -= done;
                c->tx_seq += done;
            }
            if (c->fin_sent && ack == c->fin_seq + 1u)
                c->fin_acked = 1;
            c->una = ack;
            c->progress = now_ms();
        }
        c->wnd = get16(p + 14);
    }
    if (dlen || (fl & 0x01)) {
        if (seq == c->rcv_nxt) {
            uint32_t take = dlen, room = TCP_BUF - c->rx_len;
            if (take > room)
                take = room;
            memcpy(c->rx + c->rx_len, p + hl, take);
            c->rx_len += take;
            c->rcv_nxt += take;
            if ((fl & 0x01) && take == dlen && !c->console_fin) {
                c->rcv_nxt++;
                c->console_fin = 1;
            }
        }
        tcp_segment(n, c, c->nxt, 0x10, NULL, 0);          /* ACK what we have */
        tcp_flush_rx(n, c);
        if (!c->used)
            return;
    }
    tcp_pump(n, c);
    if (c->fin_acked && c->console_fin && !c->rx_len)
        tcp_close(n, c, "closed");
}

/* This PC's socket, from the router's select(). */
static void tcp_service(XboxNat *n, NatTcp *c, int readable, int writable, int failed)
{
    char s[16];

    if (c->connecting) {
        int err = 0;
        socklen_t el = sizeof err;
        if (!writable && !failed)
            return;
        if (getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err || failed) {
            say(n, "TCP to %s port %u: this PC could not connect, refused",
                ip_text(c->remote_ip, s, sizeof s), c->remote_port);
            tcp_segment(n, c, c->iss, 0x14, NULL, 0);
            tcp_close(n, c, NULL);
            return;
        }
        c->connecting = 0;
        if (n->verbose)
            say(n, "TCP to %s port %u: connected", ip_text(c->remote_ip, s, sizeof s),
                c->remote_port);
        tcp_pump(n, c);
        return;
    }
    if (readable && !c->host_eof) {
        uint32_t room = TCP_BUF - c->tx_len;
        if (room) {
            int r = (int)recv(c->sock, (char *)c->tx + c->tx_len, (int)room, 0);
            if (r > 0) {
                c->tx_len += (uint32_t)r;
                c->bytes_in += (unsigned long)r;
                c->last = now_ms();
            } else if (r == 0 || !NET_WOULD_BLOCK()) {
                c->host_eof = 1;
            }
        }
    }
    if (writable)
        tcp_flush_rx(n, c);
    if (c->used)
        tcp_pump(n, c);
}

/* Go back to the oldest unacknowledged byte when the console has said
 * nothing for a while, and close what has finished or gone quiet. */
static void tcp_timers(XboxNat *n)
{
    unsigned long long now = now_ms();
    int i;

    for (i = 0; i < TCP_MAX; i++) {
        NatTcp *c = &n->tcp[i];
        if (!c->used)
            continue;
        if (now - c->last > TCP_IDLE_MS) {
            tcp_abort(n, c, "idle");
            continue;
        }
        if (!c->connecting && c->una != c->nxt && now - c->progress >= TCP_RTO_MS) {
            c->nxt = c->una;
            if (c->fin_sent && c->una == c->fin_seq)
                c->fin_sent = 0;
            c->progress = now;
            tcp_pump(n, c);
        }
    }
}

static void icmp_in(XboxNat *n, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t plen)
{
    uint8_t *e = n->scratch;

    if (plen < 8 || p[0] != 8 || dst != XBOX_NAT_GATEWAY || plen > sizeof n->scratch)
        return;
    memcpy(e, p, plen);
    e[0] = 0;                             /* echo reply */
    put16(e + 2, 0);
    put16(e + 2, fold(sum16(e, plen, 0)));
    deliver_ip(n, n->console_mac, IP_ICMP, dst, src, e, plen);
}

/* ---- IPv4 -------------------------------------------------------------- */

/* A fragment into its datagram. The whole datagram once every byte is in,
 * else NULL. Fragments are assumed not to overlap, which XNet's do not. */
static const uint8_t *reassemble(XboxNat *n, uint32_t src, uint32_t dst, uint16_t id,
                                 uint8_t proto, uint32_t off, int more,
                                 const uint8_t *p, uint32_t plen, uint32_t *out_len)
{
    unsigned long long now = now_ms();
    Frag *f = NULL, *oldest = NULL;
    int i;

    for (i = 0; i < FRAG_SLOTS; i++) {
        Frag *c = &n->frag[i];
        if (c->used && now - c->t > FRAG_IDLE_MS)
            c->used = 0;
        if (c->used && c->src == src && c->dst == dst && c->id == id && c->proto == proto)
            f = c;
        if (!oldest || !c->used || (oldest->used && c->t < oldest->t))
            oldest = c;
    }
    if (!f) {
        f = oldest;
        memset(f, 0, offsetof(Frag, data));
        f->used = 1;
        f->src = src;
        f->dst = dst;
        f->id = id;
        f->proto = proto;
    }
    f->t = now;
    if (off + plen > FRAG_MAX) {
        f->used = 0;
        n->dropped++;
        return NULL;
    }
    memcpy(f->data + off, p, plen);
    f->have += plen;
    if (!more)
        f->total = off + plen;
    if (!f->total || f->have < f->total)
        return NULL;
    f->used = 0;          /* data stays good until the next fragment */
    *out_len = f->total;
    return f->data;
}

static void ipv4_in(XboxNat *n, const uint8_t *ip, uint32_t len)
{
    uint32_t ihl, tot, src, dst, off, plen;
    uint16_t fl;
    const uint8_t *p;

    if (len < 20 || (ip[0] >> 4) != 4)
        return;
    ihl = (ip[0] & 15u) * 4u;
    tot = get16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > len)
        return;
    fl = get16(ip + 6);
    off = (fl & 0x1FFFu) * 8u;
    src = get32(ip + 12);
    dst = get32(ip + 16);
    p = ip + ihl;
    plen = tot - ihl;
    if ((fl & 0x2000u) || off) {
        p = reassemble(n, src, dst, get16(ip + 4), ip[9], off, (fl & 0x2000u) != 0,
                       p, plen, &plen);
        if (!p)
            return;
    }
    switch (ip[9]) {
    case IP_UDP:  udp_in(n, src, dst, p, plen); break;
    case IP_TCP:  tcp_in(n, src, dst, p, plen); break;
    case IP_ICMP: icmp_in(n, src, dst, p, plen); break;
    default:      break;
    }
}

static void frame_in(XboxNat *n, const uint8_t *f, uint32_t len)
{
    if (len < 14)
        return;
    pcap_frame(n, f, len);
    switch (get16(f + 12)) {
    case ETH_ARP:  arp_in(n, f + 14, len - 14); break;
    case ETH_IPV4: ipv4_in(n, f + 14, len - 14); break;
    default:       break;
    }
}

/* ---- the router's thread ----------------------------------------------- */

static void from_internet(XboxNat *n, NatUdp *m)
{
    static uint8_t buf[65536];            /* the router thread's alone */
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int r;

    r = (int)recvfrom(m->sock, (char *)buf, (int)sizeof buf, 0,
                      (struct sockaddr *)&from, &fl);
    if (r < 0)
        return;
    m->last = now_ms();
    if (n->verbose) {
        char s[16];
        say(n, "UDP %d bytes from %s:%u for the console's port %u", r,
            ip_text(ntohl(from.sin_addr.s_addr), s, sizeof s),
            ntohs(from.sin_port), m->console_port);
    }
    deliver_udp(n, n->console_mac, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port),
                m->console_ip, m->console_port, buf, (uint32_t)r);
}

#ifdef _WIN32
static DWORD WINAPI router_thread(LPVOID arg)
#else
static void *router_thread(void *arg)
#endif
{
    XboxNat *n = (XboxNat *)arg;
    Slot frame;
    unsigned long long last_sweep = now_ms();

    while (!n->stop) {
        fd_set rd, wr, ex;
        struct timeval tv;
        net_socket hi = n->wake;
        int i, r;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(n->wake, &rd);
        for (i = 0; i < UDP_MAX; i++)
            if (n->udp[i].used) {
                FD_SET(n->udp[i].sock, &rd);
                if (n->udp[i].sock > hi)
                    hi = n->udp[i].sock;
            }
        for (i = 0; i < TCP_MAX; i++) {
            NatTcp *c = &n->tcp[i];
            if (!c->used)
                continue;
            if (c->connecting) {
                FD_SET(c->sock, &wr);
                FD_SET(c->sock, &ex);
            } else {
                if (!c->host_eof && c->tx_len < TCP_BUF)
                    FD_SET(c->sock, &rd);
                if (c->rx_len)
                    FD_SET(c->sock, &wr);
            }
            if (c->sock > hi)
                hi = c->sock;
        }
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        r = select((int)hi + 1, &rd, &wr, &ex, &tv);

        if (r > 0 && FD_ISSET(n->wake, &rd)) {
            char b[64];
            recv(n->wake, b, (int)sizeof b, 0);
        }
        /* The card's frames, every one queued, whether or not the wake-up
         * byte has arrived yet. */
        for (;;) {
            lock(n);
            if (!n->out_count) {
                unlock(n);
                break;
            }
            frame = n->out[n->out_head];
            n->out_head = (n->out_head + 1) % OUT_LEN;
            n->out_count--;
            unlock(n);
            frame_in(n, frame.data, frame.len);
        }
        if (r > 0)
            for (i = 0; i < UDP_MAX; i++)
                if (n->udp[i].used && FD_ISSET(n->udp[i].sock, &rd))
                    from_internet(n, &n->udp[i]);
        if (r > 0)
            for (i = 0; i < TCP_MAX; i++) {
                NatTcp *c = &n->tcp[i];
                if (c->used && c->sock != NET_BAD_SOCKET)
                    tcp_service(n, c, FD_ISSET(c->sock, &rd), FD_ISSET(c->sock, &wr),
                                FD_ISSET(c->sock, &ex));
            }
        tcp_timers(n);

        if (now_ms() - last_sweep >= 10000u) {
            unsigned long long now = now_ms();
            last_sweep = now;
            for (i = 0; i < UDP_MAX; i++)
                if (n->udp[i].used && now - n->udp[i].last > UDP_IDLE_MS) {
                    if (n->verbose)
                        say(n, "UDP from the console's port %u: idle, closed",
                            n->udp[i].console_port);
                    udp_close(&n->udp[i]);
                }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- the API ----------------------------------------------------------- */

static int parse_dns(XboxNat *n, const char *list)
{
    char buf[128], *p, *next;

    n->ndns = 0;
    snprintf(buf, sizeof buf, "%s", list);
    for (p = buf; p && *p && n->ndns < 4; p = next) {
        struct in_addr a;
        next = strchr(p, ',');
        if (next)
            *next++ = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (inet_pton(AF_INET, p, &a) == 1)
            n->dns[n->ndns++] = ntohl(a.s_addr);
        else if (*p)
            say(n, "net_dns: %s is not an IPv4 address; left out", p);
    }
    return n->ndns;
}

XboxNat *xbox_net_nat_open(const XboxNatConfig *cfg, const uint8_t own_mac[6])
{
    XboxNat *n;
    socklen_t al = sizeof(struct sockaddr_in);
    const char *v;
    int i;

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return NULL;
    }
#endif
    n = (XboxNat *)calloc(1, sizeof *n);
    if (!n)
        return NULL;
    if (cfg)
        n->cfg = *cfg;
    memcpy(n->console_mac, own_mac, 6);
    v = getenv("RECOMP_NET_LOG");
    n->verbose = v && *v && strcmp(v, "0") != 0;
    v = getenv("RECOMP_NET_PCAP");
    if (v && *v)
        pcap_open(n, v);
    if (!parse_dns(n, n->cfg.dns[0] ? n->cfg.dns : XBOX_NAT_DEFAULT_DNS))
        say(n, "no DNS server to hand out: names will not resolve");
    for (i = 0; i < UDP_MAX; i++)
        n->udp[i].sock = NET_BAD_SOCKET;
    for (i = 0; i < TCP_MAX; i++)
        n->tcp[i].sock = NET_BAD_SOCKET;

    n->wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (n->wake == NET_BAD_SOCKET)
        goto fail;
    memset(&n->wake_to, 0, sizeof n->wake_to);
    n->wake_to.sin_family = AF_INET;
    n->wake_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(n->wake, (const struct sockaddr *)&n->wake_to, (int)sizeof n->wake_to) != 0 ||
        getsockname(n->wake, (struct sockaddr *)&n->wake_to, &al) != 0)
        goto fail;

#ifdef _WIN32
    InitializeCriticalSection(&n->lock);
    n->thread = CreateThread(NULL, 0, router_thread, n, 0, NULL);
    if (!n->thread) {
        DeleteCriticalSection(&n->lock);
        goto fail;
    }
#else
    pthread_mutex_init(&n->lock, NULL);
    if (pthread_create(&n->thread, NULL, router_thread, n) != 0) {
        pthread_mutex_destroy(&n->lock);
        goto fail;
    }
    n->have_thread = 1;
#endif
    return n;

fail:
    say(n, "the router could not start (no socket or thread): no internet");
    if (n->pcap)
        fclose(n->pcap);
    if (n->wake != NET_BAD_SOCKET)
        net_close(n->wake);
    free(n);
    return NULL;
}

void xbox_net_nat_close(XboxNat *n)
{
    int i;

    if (!n)
        return;
    n->stop = 1;
    sendto(n->wake, "", 1, 0, (const struct sockaddr *)&n->wake_to, (int)sizeof n->wake_to);
#ifdef _WIN32
    WaitForSingleObject(n->thread, INFINITE);
    CloseHandle(n->thread);
    DeleteCriticalSection(&n->lock);
#else
    if (n->have_thread)
        pthread_join(n->thread, NULL);
    pthread_mutex_destroy(&n->lock);
#endif
    for (i = 0; i < UDP_MAX; i++)
        udp_close(&n->udp[i]);
    for (i = 0; i < TCP_MAX; i++)
        tcp_close(n, &n->tcp[i], NULL);
    net_close(n->wake);
    if (n->pcap)
        fclose(n->pcap);
    free(n);
}

void xbox_net_nat_send(XboxNat *n, const uint8_t *frame, uint32_t len)
{
    if (!n || len < 14 || len > XBOX_NET_FRAME_MAX)
        return;
    lock(n);
    if (n->out_count == OUT_LEN) {
        n->dropped++;
        unlock(n);
        return;
    }
    {
        Slot *s = &n->out[(n->out_head + n->out_count) % OUT_LEN];
        memcpy(s->data, frame, len);
        s->len = len;
        n->out_count++;
        n->sent++;
    }
    unlock(n);
    /* sendto is async-signal-safe; this may be the card's fault handler. */
    sendto(n->wake, "", 1, 0, (const struct sockaddr *)&n->wake_to, (int)sizeof n->wake_to);
}

uint32_t xbox_net_nat_peek(XboxNat *n, uint8_t *out, uint32_t cap)
{
    uint32_t len = 0;

    if (!n)
        return 0;
    lock(n);
    if (n->in_count) {
        len = n->in[n->in_head].len;
        if (len > cap)
            len = cap;
        memcpy(out, n->in[n->in_head].data, len);
    }
    unlock(n);
    return len;
}

void xbox_net_nat_pop(XboxNat *n)
{
    if (!n)
        return;
    lock(n);
    if (n->in_count) {
        n->in_head = (n->in_head + 1) % IN_LEN;
        n->in_count--;
    }
    unlock(n);
}

void xbox_net_nat_stats(XboxNat *n, unsigned long *sent,
                        unsigned long *received, unsigned long *dropped)
{
    if (sent) *sent = n ? n->sent : 0;
    if (received) *received = n ? n->received : 0;
    if (dropped) *dropped = n ? n->dropped : 0;
}

int xbox_net_nat_leased(const XboxNat *n)
{
    return n ? n->leased : 0;
}
