/**
 * The network card's cable: Ethernet frames over UDP.
 *
 * Three ways to be connected, chosen by RECOMP_SYSLINK (or `syslink` in the
 * title's settings file, which the launcher's System Link tab writes):
 *
 *   udp   A pair. Each frame the card sends goes out as one UDP datagram
 *         holding the bare frame, nothing added, and each datagram that
 *         arrives is a frame for the card. That is xemu's UDP tunnel (QEMU's
 *         datagram socket backend), so an xemu on the other end, or XLink
 *         Kai's xemu port at 127.0.0.1:34523, needs nothing from us but an
 *         address. Two consoles, no more.
 *
 *             RECOMP_SYSLINK_REMOTE=127.0.0.1:9002    where frames go
 *             RECOMP_SYSLINK_LOCAL=9001               the port to receive on
 *                                                     (default: any free port)
 *
 *   host  This copy is the network switch for a session of up to
 *         XBOX_NET_MAX_CONSOLES consoles, its own included. It listens on one
 *         port; consoles join it, and every frame goes through it: a
 *         broadcast (how a game says "I'm hosting") to everyone, a frame for
 *         one console to that console alone, once the switch has seen which
 *         joined copy has which Ethernet address. Only the host needs a port
 *         its friends can reach; the router is asked to open it
 *         (net_upnp.c) unless syslink_upnp is 0.
 *
 *   join  One console in a hosted session: frames to the host, frames from
 *         the host. Nothing to open on this side's router -- the host answers
 *         to the address the join came from, and a ping every two seconds
 *         keeps that way back open.
 *
 *   nat   Not a cable to other consoles but to the internet, through a
 *         router inside the runtime, for Xbox Live (Insignia). See
 *         xbox_net_nat.c; the card's frames go there instead.
 *
 *     syslink = host | join | udp | nat | off        [RECOMP_SYSLINK]
 *     syslink_host = 192.168.1.20 or name[:port]     [RECOMP_SYSLINK_HOST]
 *     syslink_port = 27610                           [RECOMP_SYSLINK_PORT]
 *     syslink_code = <anything>                      [RECOMP_SYSLINK_CODE]
 *     syslink_upnp = 1                               [RECOMP_SYSLINK_UPNP]
 *
 * host and join speak a small format of their own, not xemu's, because a
 * switch has to know who is in: four bytes ('X' 'L', version, type) ahead
 * of each message. A join says hello with the session's join code and its
 * Ethernet address; the host welcomes it or says why not (wrong code, full,
 * the same console identity as one already in). Until then nothing it sends
 * reaches anyone, so an open port does not let a stranger put frames into
 * the game's network code.
 *
 * And for trying a title at internet distances on one PC, applied to
 * frames as they are delivered to this copy's card, in every mode:
 *
 *     RECOMP_SYSLINK_DELAY_MS=80    hold every frame back this long
 *     RECOMP_SYSLINK_LOSS=2         drop this percentage of them
 *
 * Delivered frames wait in a queue until the card has a receive buffer for
 * them; the card takes them on the kernel's timer thread. The host forwards
 * on its receive thread, the moment a frame arrives, so a frame between two
 * joined consoles never waits for the host's game.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_net_udp.h"
#include "xbox_net_nat.h"
#include "recomp_config.h"
#include "net_upnp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET net_socket;
#define NET_BAD_SOCKET INVALID_SOCKET
#define net_close closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <strings.h>
typedef int net_socket;
#define NET_BAD_SOCKET (-1)
#define net_close close
#define _stricmp strcasecmp
#endif

#define QUEUE_LEN       256
#define MAX_PEERS       (XBOX_NET_MAX_CONSOLES - 1)
#define HELLO_EVERY_MS  1000u
#define RETRY_REFUSED_MS 5000u  /* after a refusal that will not change by itself */
#define PING_EVERY_MS   2000u
#define SILENT_MS       15000u  /* no word for this long: gone */
#define RTT_REPORT_MS   30000u

/* The session format: 'X' 'L', version, type, then the message. */
#define PROTO_VERSION   1
#define HDR             4
enum { MSG_HELLO = 1, MSG_WELCOME, MSG_REFUSE, MSG_FRAME, MSG_PING, MSG_PONG, MSG_BYE };
enum { REFUSE_CODE = 1, REFUSE_FULL, REFUSE_VERSION, REFUSE_SAME_CONSOLE, REFUSE_NOT_JOINED };

typedef struct Peer {
    int                used;
    struct sockaddr_in addr;
    uint8_t            mac[6];
    int                has_mac;
    unsigned long long last_seen;
} Peer;

struct XboxNetLink {
    XboxNetConfig      cfg;
    net_socket         sock;
    struct sockaddr_in remote;          /* PAIR: where frames go; JOIN: the host */
    int                port;            /* bound */
    uint8_t            own_mac[6];
    uint32_t           rng;

    /* Frames for this copy's card, oldest first. */
    struct { uint32_t len; unsigned long long due; uint8_t data[XBOX_NET_FRAME_MAX]; }
                       q[QUEUE_LEN];
    unsigned           head, count;
    unsigned long      sent, received, dropped;

    /* HOST: who is in. */
    Peer               peers[MAX_PEERS];

    /* JOIN: where it stands with the host. */
    int                joined;
    int                consoles;        /* as the host last said */
    int                refused;         /* the last refusal, 0 for none */
    unsigned long long last_hello, last_ping, last_from_host, last_rtt_report;
    long               rtt_ms;

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

static XboxNetLink *s_link;     /* the one xbox_NetUdpStart opens */
static XboxNat     *s_nat;      /* or, for syslink = nat, the router */

static void lock(XboxNetLink *l)
{
#ifdef _WIN32
    EnterCriticalSection(&l->lock);
#else
    pthread_mutex_lock(&l->lock);
#endif
}

static void unlock(XboxNetLink *l)
{
#ifdef _WIN32
    LeaveCriticalSection(&l->lock);
#else
    pthread_mutex_unlock(&l->lock);
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
static void say(const XboxNetLink *l, const char *fmt, ...)
{
    va_list ap;

    if (l->cfg.quiet)
        return;
    va_start(ap, fmt);
    fputs("[NET] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static const char *addr_text(const struct sockaddr_in *a, char *out, size_t n)
{
    const unsigned char *b = (const unsigned char *)&a->sin_addr;

    snprintf(out, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(a->sin_port));
    return out;
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* xorshift32: loss injection wants something cheap, not something good. */
static uint32_t next_random(XboxNetLink *l)
{
    uint32_t x = l->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return l->rng = x;
}

/* ---- the switch ------------------------------------------------------- */

int xbox_net_route(const uint8_t dest[6], const uint8_t own_mac[6],
                   const uint8_t (*peer_mac)[6], const int *has_mac, int peers,
                   int from, int *to_local, int *to)
{
    int i, n = 0;

    *to_local = 0;
    /* Broadcast and multicast -- the group bit, first on the wire -- go to
     * every console but the one that sent it. */
    if (dest[0] & 1) {
        *to_local = from >= 0;
        for (i = 0; i < peers; i++)
            if (i != from)
                to[n++] = i;
        return n;
    }
    if (memcmp(dest, own_mac, 6) == 0) {
        *to_local = from >= 0;
        return 0;
    }
    for (i = 0; i < peers; i++)
        if (i != from && has_mac[i] && memcmp(peer_mac[i], dest, 6) == 0) {
            to[0] = i;
            return 1;
        }
    /* An address the switch has not seen yet: everyone but the sender, as a
     * learning switch floods. Not the host's own card, whose address is
     * known and is not this one. */
    for (i = 0; i < peers; i++)
        if (i != from)
            to[n++] = i;
    return n;
}

/* ---- delivery to this copy's card ------------------------------------ */

static void deliver(XboxNetLink *l, const uint8_t *frame, int n)
{
    /* Not a frame, or one of ours coming back (a relay that echoes, or a
     * misconfigured pair pointed at itself). */
    if (n < 14 || n > (int)XBOX_NET_FRAME_MAX || memcmp(frame + 6, l->own_mac, 6) == 0)
        return;
    lock(l);
    if (l->cfg.loss_pct > 0 && (int)(next_random(l) % 100u) < l->cfg.loss_pct) {
        l->dropped++;
    } else if (l->count == QUEUE_LEN) {
        l->dropped++;
    } else {
        unsigned i = (l->head + l->count) % QUEUE_LEN;
        l->q[i].len = (uint32_t)n;
        l->q[i].due = now_ms() + (unsigned long long)l->cfg.delay_ms;
        memcpy(l->q[i].data, frame, (size_t)n);
        l->count++;
        l->received++;
    }
    unlock(l);
}

/* ---- the session format ---------------------------------------------- */

static void send_msg(XboxNetLink *l, const struct sockaddr_in *to, int type,
                     const void *body, size_t len)
{
    uint8_t buf[HDR + XBOX_NET_FRAME_MAX + 80];

    if (len > sizeof buf - HDR)
        return;
    buf[0] = 'X';
    buf[1] = 'L';
    buf[2] = PROTO_VERSION;
    buf[3] = (uint8_t)type;
    if (len)
        memcpy(buf + HDR, body, len);
    sendto(l->sock, (const char *)buf, (int)(HDR + len), 0,
           (const struct sockaddr *)to, (int)sizeof *to);
}

static void send_hello(XboxNetLink *l)
{
    uint8_t body[1 + 63 + 6];
    size_t cl = strlen(l->cfg.code);

    if (cl > 63)
        cl = 63;
    body[0] = (uint8_t)cl;
    memcpy(body + 1, l->cfg.code, cl);
    memcpy(body + 1 + cl, l->own_mac, 6);
    send_msg(l, &l->remote, MSG_HELLO, body, 1 + cl + 6);
    l->last_hello = now_ms();
}

static int consoles_in(const XboxNetLink *l)
{
    int i, n = 1;

    for (i = 0; i < MAX_PEERS; i++)
        n += l->peers[i].used;
    return n;
}

static const char *refusal_text(int why)
{
    switch (why) {
    case REFUSE_CODE:         return "the join code does not match the host's";
    case REFUSE_FULL:         return "the session is full";
    case REFUSE_VERSION:      return "the host's build speaks a different version; "
                                     "everyone needs the same toolkit";
    case REFUSE_SAME_CONSOLE: return "this console has the same identity as one already "
                                     "in (two copies sharing one save folder?)";
    case REFUSE_NOT_JOINED:   return "the host does not know this console (it restarted?)";
    default:                  return "no reason given";
    }
}

/* Under the lock: the frame from peer `from` (-1: the host's own card) out
 * to wherever the switch says. Returns whether the host's card takes it. */
static int forward(XboxNetLink *l, const uint8_t *frame, uint32_t len, int from)
{
    uint8_t macs[MAX_PEERS][6];
    int has[MAX_PEERS], slot[MAX_PEERS], to[MAX_PEERS];
    int i, n = 0, k, from_index = -1, local = 0;

    for (i = 0; i < MAX_PEERS; i++) {
        if (!l->peers[i].used)
            continue;
        if (i == from)
            from_index = n;
        memcpy(macs[n], l->peers[i].mac, 6);
        has[n] = l->peers[i].has_mac;
        slot[n] = i;
        n++;
    }
    k = xbox_net_route(frame, l->own_mac, (const uint8_t (*)[6])macs, has, n,
                       from_index, &local, to);
    for (i = 0; i < k; i++) {
        send_msg(l, &l->peers[slot[to[i]]].addr, MSG_FRAME, frame, len);
        l->sent++;
    }
    return local;
}

static void host_receive(XboxNetLink *l, const uint8_t *buf, int n,
                         const struct sockaddr_in *from)
{
    char who[32];
    int i, p = -1, local = 0;
    unsigned long long now = now_ms();

    if (n < HDR || buf[0] != 'X' || buf[1] != 'L')
        return;
    lock(l);
    for (i = 0; i < MAX_PEERS; i++)
        if (l->peers[i].used && same_addr(&l->peers[i].addr, from)) {
            p = i;
            break;
        }

    if (buf[3] == MSG_HELLO) {
        uint8_t refuse = 0, welcome[1];
        size_t cl;
        char code[64];
        const uint8_t *mac = NULL;

        /* A hello is the code's length, the code, the Ethernet address. A
         * build that speaks another version is told so before any of that
         * is read, since its hello need not have this shape. */
        if (buf[2] != PROTO_VERSION) {
            refuse = REFUSE_VERSION;
        } else {
            if (n < HDR + 1)
                goto done;
            cl = buf[HDR];
            if (cl > 63 || (size_t)n < HDR + 1 + cl + 6)
                goto done;              /* not a hello */
            memcpy(code, buf + HDR + 1, cl);
            code[cl] = 0;
            mac = buf + HDR + 1 + cl;
            if (_stricmp(code, l->cfg.code) != 0)
                refuse = REFUSE_CODE;
            else if (memcmp(mac, l->own_mac, 6) == 0)
                refuse = REFUSE_SAME_CONSOLE;
        }
        if (!refuse && p < 0) {
            /* The same console back from a new address -- its copy was
             * restarted -- takes its old place. */
            for (i = 0; i < MAX_PEERS; i++)
                if (l->peers[i].used && l->peers[i].has_mac &&
                    memcmp(l->peers[i].mac, mac, 6) == 0) {
                    p = i;
                    break;
                }
            if (p < 0)
                for (i = 0; i < MAX_PEERS; i++)
                    if (!l->peers[i].used) {
                        p = i;
                        break;
                    }
            if (p < 0) {
                refuse = REFUSE_FULL;
            } else {
                int back = l->peers[p].used;

                l->peers[p].used = 1;
                l->peers[p].addr = *from;
                memcpy(l->peers[p].mac, mac, 6);
                l->peers[p].has_mac = 1;
                l->peers[p].last_seen = now;
                say(l, "a console %s from %s: %d in the session",
                    back ? "rejoined" : "joined", addr_text(from, who, sizeof who),
                    consoles_in(l));
            }
        }
        if (refuse) {
            send_msg(l, from, MSG_REFUSE, &refuse, 1);
            say(l, "refused a console at %s: %s", addr_text(from, who, sizeof who),
                refusal_text(refuse));
            goto done;
        }
        l->peers[p].last_seen = now;
        welcome[0] = (uint8_t)consoles_in(l);
        send_msg(l, from, MSG_WELCOME, welcome, 1);
        goto done;
    }

    if (p < 0) {
        /* Anything but a hello from a console that is not in. Tell it, so
         * one that thinks it joined a host that has since restarted says
         * hello again -- but put nothing it sent anywhere. */
        if (buf[3] == MSG_FRAME || buf[3] == MSG_PING) {
            uint8_t why = REFUSE_NOT_JOINED;
            send_msg(l, from, MSG_REFUSE, &why, 1);
        }
        goto done;
    }
    l->peers[p].last_seen = now;

    switch (buf[3]) {
    case MSG_FRAME:
        if (n - HDR < 14 || n - HDR > (int)XBOX_NET_FRAME_MAX)
            break;
        /* The switch learns where an address is from what comes from it. */
        if (memcmp(l->peers[p].mac, buf + HDR + 6, 6) != 0) {
            memcpy(l->peers[p].mac, buf + HDR + 6, 6);
            l->peers[p].has_mac = 1;
        }
        local = forward(l, buf + HDR, (uint32_t)(n - HDR), p);
        break;
    case MSG_PING:
        send_msg(l, from, MSG_PONG, buf + HDR, (size_t)(n - HDR));
        break;
    case MSG_BYE:
        l->peers[p].used = 0;
        say(l, "the console at %s left: %d in the session",
            addr_text(from, who, sizeof who), consoles_in(l));
        break;
    default:
        break;
    }
done:
    unlock(l);
    if (local)
        deliver(l, buf + HDR, n - HDR);
}

static void join_receive(XboxNetLink *l, const uint8_t *buf, int n,
                         const struct sockaddr_in *from)
{
    char who[32];
    unsigned long long now = now_ms();

    if (!same_addr(from, &l->remote) || n < HDR || buf[0] != 'X' || buf[1] != 'L')
        return;
    l->last_from_host = now;
    switch (buf[3]) {
    case MSG_WELCOME:
        l->consoles = n > HDR ? buf[HDR] : 0;
        if (!l->joined)
            say(l, "joined the session at %s: %d in it",
                addr_text(&l->remote, who, sizeof who), l->consoles);
        l->joined = 1;
        l->refused = 0;
        l->last_ping = now;
        break;
    case MSG_REFUSE: {
        int why = n > HDR ? buf[HDR] : 0;

        if (why != l->refused)
            say(l, "the host at %s refused this console: %s",
                addr_text(&l->remote, who, sizeof who), refusal_text(why));
        l->refused = why;
        l->joined = 0;
        if (why == REFUSE_NOT_JOINED)
            l->last_hello = 0;          /* say hello again now */
        break;
    }
    case MSG_FRAME:
        if (l->joined)
            deliver(l, buf + HDR, n - HDR);
        break;
    case MSG_PONG:
        if (n >= HDR + 8) {
            unsigned long long sent;

            memcpy(&sent, buf + HDR, 8);
            if (sent <= now)
                l->rtt_ms = (long)(now - sent);
        }
        break;
    case MSG_BYE:
        if (l->joined)
            say(l, "the host at %s closed the session",
                addr_text(&l->remote, who, sizeof who));
        l->joined = 0;
        break;
    default:
        break;
    }
}

static void housekeeping(XboxNetLink *l)
{
    unsigned long long now = now_ms();
    char who[32];
    int i;

    if (l->cfg.mode == XBOX_NET_JOIN) {
        if (!l->joined) {
            unsigned every = (l->refused && l->refused != REFUSE_NOT_JOINED)
                             ? RETRY_REFUSED_MS : HELLO_EVERY_MS;
            if (now - l->last_hello >= every)
                send_hello(l);
            return;
        }
        if (now - l->last_from_host > SILENT_MS) {
            say(l, "the host at %s stopped answering; trying again",
                addr_text(&l->remote, who, sizeof who));
            l->joined = 0;
            return;
        }
        if (now - l->last_ping >= PING_EVERY_MS) {
            send_msg(l, &l->remote, MSG_PING, &now, sizeof now);
            l->last_ping = now;
        }
        if (now - l->last_rtt_report >= RTT_REPORT_MS && l->rtt_ms >= 0) {
            say(l, "round trip to the host %ld ms, %d in the session",
                l->rtt_ms, l->consoles);
            l->last_rtt_report = now;
        }
    } else if (l->cfg.mode == XBOX_NET_HOST) {
        lock(l);
        for (i = 0; i < MAX_PEERS; i++)
            if (l->peers[i].used && now - l->peers[i].last_seen > SILENT_MS) {
                l->peers[i].used = 0;
                say(l, "the console at %s went quiet and was dropped: %d in the session",
                    addr_text(&l->peers[i].addr, who, sizeof who), consoles_in(l));
            }
        unlock(l);
    }
}

#ifdef _WIN32
static DWORD WINAPI receive_thread(LPVOID arg)
#else
static void *receive_thread(void *arg)
#endif
{
    XboxNetLink *l = (XboxNetLink *)arg;
    uint8_t buf[2048];

    while (!l->stop) {
        fd_set rd;
        struct timeval tv;
        int r;

        FD_ZERO(&rd);
        FD_SET(l->sock, &rd);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;            /* housekeeping at least ten times a second */
        r = select((int)l->sock + 1, &rd, NULL, NULL, &tv);
        if (r > 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(l->sock, (char *)buf, (int)sizeof buf, 0,
                                  (struct sockaddr *)&from, &fl);
            if (n < 0) {
#ifdef _WIN32
                /* A datagram to a port nobody is listening on yet comes back
                 * as an ICMP error, which Windows reports on the next receive
                 * as WSAECONNRESET. It says nothing about this socket. */
                if (WSAGetLastError() == WSAECONNRESET)
                    continue;
#endif
                if (!l->stop)
                    say(l, "receive failed; the tunnel stops receiving");
                break;
            }
            if (l->cfg.mode == XBOX_NET_HOST)
                host_receive(l, buf, n, &from);
            else if (l->cfg.mode == XBOX_NET_JOIN)
                join_receive(l, buf, n, &from);
            else
                deliver(l, buf, n);
        } else if (r < 0 && !l->stop) {
            say(l, "waiting for the network failed; the tunnel stops receiving");
            break;
        }
        housekeeping(l);
    }
    return 0;
}

/* "host[:port]" to an address; the port is default_port when none is named. */
static int resolve(const char *spec, int default_port, struct sockaddr_in *out)
{
    char host[256], port[16];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints, *res = NULL;
    size_t hl = colon ? (size_t)(colon - spec) : strlen(spec);

    while (*spec == ' ') { spec++; if (hl) hl--; }
    if (hl == 0 || hl >= sizeof host)
        return 0;
    memcpy(host, spec, hl);
    host[hl] = 0;
    while (hl && host[hl - 1] == ' ')
        host[--hl] = 0;
    if (colon && colon[1])
        snprintf(port, sizeof port, "%s", colon + 1);
    else if (default_port > 0)
        snprintf(port, sizeof port, "%d", default_port);
    else
        return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return 0;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 1;
}

static int net_startup(void)
{
#ifdef _WIN32
    static int started;
    WSADATA wsa;

    if (started)
        return 1;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    started = 1;
#endif
    return 1;
}

XboxNetLink *xbox_net_link_open(const XboxNetConfig *cfg, const uint8_t own_mac[6])
{
    XboxNetLink *l;
    struct sockaddr_in bind_addr;
    socklen_t bl = sizeof bind_addr;
    int port;

    if (!cfg || cfg->mode == XBOX_NET_OFF || !net_startup())
        return NULL;
    l = (XboxNetLink *)calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->cfg = *cfg;
    l->sock = NET_BAD_SOCKET;
    l->rtt_ms = -1;
    memcpy(l->own_mac, own_mac, 6);
    l->rng = 0x9E3779B9u ^ (uint32_t)now_ms() ^ ((uint32_t)own_mac[5] << 8);
#ifdef _WIN32
    InitializeCriticalSection(&l->lock);
#else
    pthread_mutex_init(&l->lock, NULL);
#endif

    if (cfg->mode != XBOX_NET_HOST) {
        int dflt = cfg->mode == XBOX_NET_JOIN
                   ? (cfg->port > 0 ? cfg->port : XBOX_NET_DEFAULT_PORT) : 0;
        if (!cfg->remote[0] || !resolve(cfg->remote, dflt, &l->remote)) {
            say(l, "\"%s\" is not an address%s; no System Link", cfg->remote,
                cfg->mode == XBOX_NET_PAIR ? " (host:port)" : "");
            goto fail;
        }
    }
    port = cfg->mode == XBOX_NET_JOIN ? 0 : cfg->port;

    l->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (l->sock == NET_BAD_SOCKET) {
        say(l, "no UDP socket; no System Link");
        goto fail;
    }
#ifdef _WIN32
    {
        BOOL report = FALSE;
        DWORD got;
        WSAIoctl(l->sock, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0, &got,
                 NULL, NULL);
    }
#endif
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(cfg->loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    bind_addr.sin_port = htons((unsigned short)port);
    if (bind(l->sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        say(l, "cannot receive on UDP port %d (another copy already using it?); "
               "no System Link", port);
        goto fail;
    }
    getsockname(l->sock, (struct sockaddr *)&bind_addr, &bl);
    l->port = ntohs(bind_addr.sin_port);

#ifdef _WIN32
    l->thread = CreateThread(NULL, 0, receive_thread, l, 0, NULL);
    if (!l->thread) {
        say(l, "no receive thread; no System Link");
        goto fail;
    }
#else
    if (pthread_create(&l->thread, NULL, receive_thread, l) != 0) {
        say(l, "no receive thread; no System Link");
        goto fail;
    }
    l->have_thread = 1;
#endif
    return l;

fail:
    if (l->sock != NET_BAD_SOCKET)
        net_close(l->sock);
#ifdef _WIN32
    DeleteCriticalSection(&l->lock);
#else
    pthread_mutex_destroy(&l->lock);
#endif
    free(l);
    return NULL;
}

void xbox_net_link_close(XboxNetLink *l)
{
    int i;

    if (!l)
        return;
    /* Say goodbye, so the other side does not wait fifteen seconds to
     * notice. */
    if (l->cfg.mode == XBOX_NET_JOIN && l->joined)
        send_msg(l, &l->remote, MSG_BYE, NULL, 0);
    else if (l->cfg.mode == XBOX_NET_HOST)
        for (i = 0; i < MAX_PEERS; i++)
            if (l->peers[i].used)
                send_msg(l, &l->peers[i].addr, MSG_BYE, NULL, 0);
    l->stop = 1;
#ifdef _WIN32
    WaitForSingleObject(l->thread, INFINITE);
    CloseHandle(l->thread);
#else
    if (l->have_thread)
        pthread_join(l->thread, NULL);
#endif
    net_close(l->sock);
#ifdef _WIN32
    DeleteCriticalSection(&l->lock);
#else
    pthread_mutex_destroy(&l->lock);
#endif
    if (s_link == l)
        s_link = NULL;
    free(l);
}

void xbox_net_link_send(XboxNetLink *l, const uint8_t *frame, uint32_t len)
{
    if (!l || len < 14 || len > XBOX_NET_FRAME_MAX)
        return;
    switch (l->cfg.mode) {
    case XBOX_NET_PAIR:
        if (sendto(l->sock, (const char *)frame, (int)len, 0,
                   (const struct sockaddr *)&l->remote, (int)sizeof l->remote) == (int)len)
            l->sent++;
        break;
    case XBOX_NET_JOIN:
        /* Before the host has said yes, there is nobody to send to. */
        if (l->joined) {
            send_msg(l, &l->remote, MSG_FRAME, frame, len);
            l->sent++;
        }
        break;
    case XBOX_NET_HOST:
        lock(l);
        forward(l, frame, len, -1);
        unlock(l);
        break;
    default:
        break;
    }
}

uint32_t xbox_net_link_peek(XboxNetLink *l, uint8_t *out, uint32_t cap)
{
    uint32_t len = 0;

    if (!l)
        return 0;
    lock(l);
    if (l->count && l->q[l->head].due <= now_ms()) {
        len = l->q[l->head].len;
        if (len > cap)
            len = cap;
        memcpy(out, l->q[l->head].data, len);
    }
    unlock(l);
    return len;
}

void xbox_net_link_pop(XboxNetLink *l)
{
    if (!l)
        return;
    lock(l);
    if (l->count) {
        l->head = (l->head + 1) % QUEUE_LEN;
        l->count--;
    }
    unlock(l);
}

int xbox_net_link_port(const XboxNetLink *l)     { return l ? l->port : 0; }
int xbox_net_link_joined(const XboxNetLink *l)   { return l ? l->joined : 0; }

int xbox_net_link_consoles(const XboxNetLink *l)
{
    int n;

    if (!l)
        return 0;
    if (l->cfg.mode != XBOX_NET_HOST)
        return l->joined ? l->consoles : 0;
    lock((XboxNetLink *)l);
    n = consoles_in(l);
    unlock((XboxNetLink *)l);
    return n;
}

/* ---- the card's cable, as the settings say --------------------------- */

static XboxNetMode mode_from(const char *v)
{
    if (!v || !*v)
        return XBOX_NET_OFF;
    if (_stricmp(v, "udp") == 0 || _stricmp(v, "pair") == 0)
        return XBOX_NET_PAIR;
    if (_stricmp(v, "host") == 0)
        return XBOX_NET_HOST;
    if (_stricmp(v, "join") == 0)
        return XBOX_NET_JOIN;
    if (_stricmp(v, "nat") == 0)
        return XBOX_NET_NAT;
    return XBOX_NET_OFF;
}

int xbox_NetUdpStart(const uint8_t own_mac[6])
{
    XboxNetConfig cfg;
    const char *v;
    char who[32];

    if (s_link || s_nat)
        return 1;
    memset(&cfg, 0, sizeof cfg);
    cfg.mode = mode_from(recomp_config_lookup("RECOMP_SYSLINK", "syslink"));
    if (cfg.mode == XBOX_NET_OFF)
        return 0;
    if (cfg.mode == XBOX_NET_NAT) {
        XboxNatConfig nc;
        memset(&nc, 0, sizeof nc);
        if ((v = recomp_config_lookup("RECOMP_NET_DNS", "net_dns")) != NULL)
            snprintf(nc.dns, sizeof nc.dns, "%s", v);
        nc.fresh_ports = recomp_config_bool("RECOMP_NET_FRESH_PORTS", NULL, 0);
        s_nat = xbox_net_nat_open(&nc, own_mac);
        if (s_nat)
            fprintf(stderr, "[NET] the card is cabled to the internet, through "
                            "a router in the runtime (syslink = nat)\n");
        fflush(stderr);
        return s_nat != NULL;
    }
    if ((v = getenv("RECOMP_SYSLINK_DELAY_MS")) != NULL && *v)
        cfg.delay_ms = strtol(v, NULL, 10);
    if ((v = getenv("RECOMP_SYSLINK_LOSS")) != NULL && *v)
        cfg.loss_pct = atoi(v);
    if ((v = recomp_config_lookup("RECOMP_SYSLINK_CODE", "syslink_code")) != NULL)
        snprintf(cfg.code, sizeof cfg.code, "%s", v);

    switch (cfg.mode) {
    case XBOX_NET_PAIR:
        if ((v = recomp_config_lookup("RECOMP_SYSLINK_REMOTE", "syslink_remote")) != NULL)
            snprintf(cfg.remote, sizeof cfg.remote, "%s", v);
        cfg.port = recomp_config_int("RECOMP_SYSLINK_LOCAL", NULL, 0);
        break;
    case XBOX_NET_HOST:
        cfg.port = recomp_config_int("RECOMP_SYSLINK_PORT", "syslink_port",
                                     XBOX_NET_DEFAULT_PORT);
        break;
    case XBOX_NET_JOIN:
        if ((v = recomp_config_lookup("RECOMP_SYSLINK_HOST", "syslink_host")) != NULL)
            snprintf(cfg.remote, sizeof cfg.remote, "%s", v);
        cfg.port = recomp_config_int("RECOMP_SYSLINK_PORT", "syslink_port",
                                     XBOX_NET_DEFAULT_PORT);
        break;
    default:
        break;
    }

    s_link = xbox_net_link_open(&cfg, own_mac);
    if (!s_link)
        return 0;

    switch (cfg.mode) {
    case XBOX_NET_PAIR:
        fprintf(stderr, "[NET] System Link over UDP: receiving on port %d, sending to %s",
                s_link->port, addr_text(&s_link->remote, who, sizeof who));
        break;
    case XBOX_NET_HOST: {
        char lan[64];

        fprintf(stderr, "[NET] hosting System Link on UDP port %d, up to %d consoles%s",
                s_link->port, XBOX_NET_MAX_CONSOLES,
                cfg.code[0] ? ", with a join code" : "");
        if (net_local_address(lan, sizeof lan))
            fprintf(stderr, "; on this network friends join %s:%d", lan, s_link->port);
        /* The internet address comes from the router, if it answers. */
        if (recomp_config_bool("RECOMP_SYSLINK_UPNP", "syslink_upnp", 1))
            net_upnp_open_async(s_link->port);
        else
            fprintf(stderr, "; the router is not asked to open the port (syslink_upnp = 0)");
        break;
    }
    case XBOX_NET_JOIN:
        fprintf(stderr, "[NET] joining the System Link session at %s%s",
                addr_text(&s_link->remote, who, sizeof who),
                cfg.code[0] ? " with a join code" : "");
        break;
    default:
        break;
    }
    if (cfg.delay_ms || cfg.loss_pct)
        fprintf(stderr, " (adding %ld ms and %d%% loss to what arrives)",
                cfg.delay_ms, cfg.loss_pct);
    fprintf(stderr, "\n");
    fflush(stderr);
    return 1;
}

void xbox_NetUdpSend(const uint8_t *frame, uint32_t len)
{
    if (s_nat)
        xbox_net_nat_send(s_nat, frame, len);
    else
        xbox_net_link_send(s_link, frame, len);
}

uint32_t xbox_NetUdpPeek(uint8_t *out, uint32_t cap)
{
    if (s_nat)
        return xbox_net_nat_peek(s_nat, out, cap);
    return xbox_net_link_peek(s_link, out, cap);
}

void xbox_NetUdpPop(void)
{
    if (s_nat)
        xbox_net_nat_pop(s_nat);
    else
        xbox_net_link_pop(s_link);
}

void xbox_NetUdpStats(unsigned long *sent, unsigned long *received,
                      unsigned long *dropped)
{
    if (s_nat) {
        xbox_net_nat_stats(s_nat, sent, received, dropped);
        return;
    }
    if (sent) *sent = s_link ? s_link->sent : 0;
    if (received) *received = s_link ? s_link->received : 0;
    if (dropped) *dropped = s_link ? s_link->dropped : 0;
}
