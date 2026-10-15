/*
 * The router behind syslink = nat (src/kernel/xbox_net_nat.c), with no game:
 * frames are built here as XNet would send them, and what comes back is
 * checked as XNet would read it. The "internet" is a UDP socket on
 * 127.0.0.1. Portable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_net_nat.h"
#include "xbox_net_udp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
typedef int sock_t;
static void pause_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&t, NULL);
}
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t k_mac[6] = { 0x00, 0x50, 0xF2, 0x12, 0x34, 0x56 };
#define LOOPBACK 0x7F000001u

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t s)
{
    for (; len > 1; p += 2, len -= 2)
        s += (uint32_t)(p[0] << 8 | p[1]);
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

/* An IPv4 frame from the console, one fragment of it if frag_off/more say so. */
static uint32_t ip_frame(uint8_t *f, const uint8_t dst_mac[6], uint8_t proto,
                         uint32_t src, uint32_t dst, uint16_t id, uint32_t frag_off,
                         int more, const uint8_t *payload, uint32_t plen)
{
    uint8_t *ip = f + 14;
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, k_mac, 6);
    put16(f + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45;
    put16(ip + 2, 20 + plen);
    put16(ip + 4, id);
    put16(ip + 6, (more ? 0x2000u : 0) | frag_off / 8u);
    ip[8] = 64;
    ip[9] = proto;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, fold(sum16(ip, 20, 0)));
    memcpy(ip + 20, payload, plen);
    return 14 + 20 + plen;
}

static uint32_t udp_payload(uint8_t *u, uint16_t sport, uint16_t dport,
                            const uint8_t *data, uint32_t len)
{
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, 8 + len);
    put16(u + 6, 0);                      /* no checksum: allowed on IPv4 */
    memcpy(u + 8, data, len);
    return 8 + len;
}

/* The next frame for the card, waiting up to a second for it. */
static uint32_t next_frame(XboxNat *n, uint8_t *out)
{
    int i;
    for (i = 0; i < 200; i++) {
        uint32_t len = xbox_net_nat_peek(n, out, XBOX_NET_FRAME_MAX);
        if (len) {
            xbox_net_nat_pop(n);
            return len;
        }
        pause_ms(5);
    }
    return 0;
}

static int udp_checksum_ok(const uint8_t *ip, const uint8_t *u, uint32_t ulen)
{
    uint32_t s = sum16(u, ulen, 0);
    uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    if (get16(u + 6) == 0)
        return 1;
    s += (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 17u + ulen;
    return fold(s) == 0;
}

/* ---- DHCP ---- */

static uint32_t dhcp_frame(uint8_t *f, int type, uint32_t want)
{
    uint8_t b[300], u[400];
    static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
    uint8_t *o;
    memset(b, 0, sizeof b);
    b[0] = 1; b[1] = 1; b[2] = 6;
    put32(b + 4, 0x12345678u);
    put16(b + 10, 0x8000);                /* broadcast, please */
    memcpy(b + 28, k_mac, 6);
    put32(b + 236, 0x63825363u);
    o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (want) { *o++ = 50; *o++ = 4; put32(o, want); o += 4; }
    *o++ = 255;
    return ip_frame(f, bcast, 17, 0, 0xFFFFFFFFu, 1, 0, 0, u,
                    udp_payload(u, 68, 67, b, sizeof b));
}

/* The reply's DHCP message type, its yiaddr, and the first DNS server. */
static int dhcp_reply(const uint8_t *f, uint32_t len, uint32_t *yiaddr, uint32_t *dns0)
{
    const uint8_t *ip = f + 14, *u = ip + 20, *b = u + 8;
    uint32_t i, blen;
    int type = 0;
    if (len < 14 + 20 + 8 + 240 || get16(u) != 67 || get16(u + 2) != 68)
        return 0;
    blen = get16(u + 4) - 8u;
    *yiaddr = get32(b + 16);
    *dns0 = 0;
    for (i = 240; i < blen && b[i] != 255; ) {
        if (b[i] == 0) { i++; continue; }
        if (b[i] == 53) type = b[i + 2];
        if (b[i] == 6 && b[i + 1] >= 4) *dns0 = get32(b + i + 2);
        i += 2u + b[i + 1];
    }
    return type;
}

static void test_dhcp_and_arp(XboxNat *n)
{
    uint8_t f[XBOX_NET_FRAME_MAX], r[XBOX_NET_FRAME_MAX];
    uint32_t len, yi, dns;
    static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };

    xbox_net_nat_send(n, f, dhcp_frame(f, 1, 0));
    len = next_frame(n, r);
    CHECK(len > 0, "no answer to DHCPDISCOVER");
    CHECK(dhcp_reply(r, len, &yi, &dns) == 2, "not an OFFER");
    CHECK(yi == XBOX_NAT_CONSOLE, "offered %08X", yi);
    CHECK(dns == 0x2E6540AFu, "first DNS %08X, not Insignia's 46.101.64.175", dns);
    CHECK(memcmp(r, bcast, 6) == 0, "OFFER not broadcast though the flag asked");
    CHECK(get32(r + 14 + 12) == XBOX_NAT_GATEWAY, "OFFER not from the router");
    CHECK(udp_checksum_ok(r + 14, r + 34, get16(r + 38)), "OFFER UDP checksum");
    CHECK(fold(sum16(r + 14, 20, 0)) == 0, "OFFER IP checksum");
    CHECK(!xbox_net_nat_leased(n), "leased before a REQUEST");

    xbox_net_nat_send(n, f, dhcp_frame(f, 3, XBOX_NAT_CONSOLE));
    len = next_frame(n, r);
    CHECK(dhcp_reply(r, len, &yi, &dns) == 5, "REQUEST not ACKed");
    CHECK(xbox_net_nat_leased(n), "not leased after the ACK");

    xbox_net_nat_send(n, f, dhcp_frame(f, 3, 0x0A000263u));
    len = next_frame(n, r);
    CHECK(dhcp_reply(r, len, &yi, &dns) == 6, "a request for another address not NAKed");

    /* ARP: the router answers for itself, and nothing answers XNet's probe
     * of its own new address. */
    {
        uint8_t *a = f + 14;
        memcpy(f, bcast, 6);
        memcpy(f + 6, k_mac, 6);
        put16(f + 12, 0x0806);
        put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 1);
        memcpy(a + 8, k_mac, 6);
        put32(a + 14, XBOX_NAT_CONSOLE);
        memset(a + 18, 0, 6);
        put32(a + 24, XBOX_NAT_GATEWAY);
        xbox_net_nat_send(n, f, 42);
        len = next_frame(n, r);
        CHECK(len == 42 && get16(r + 12) == 0x0806 && get16(r + 20) == 2,
              "no ARP reply for the router");
        CHECK(len == 42 && get32(r + 28) == XBOX_NAT_GATEWAY &&
              memcmp(r, k_mac, 6) == 0, "ARP reply wrong");

        put32(a + 14, 0);
        put32(a + 24, XBOX_NAT_CONSOLE);
        xbox_net_nat_send(n, f, 42);
        pause_ms(100);
        CHECK(xbox_net_nat_peek(n, r, sizeof r) == 0, "the console's own probe was answered");
    }
}

/* ---- UDP to and from the "internet" ---- */

static sock_t open_server(uint16_t *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    DWORD tmo = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
    struct timeval tmo = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#endif
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(LOOPBACK);
    bind(s, (struct sockaddr *)&a, sizeof a);
    getsockname(s, (struct sockaddr *)&a, &al);
    *port = ntohs(a.sin_port);
    return s;
}

static void test_udp(XboxNat *n)
{
    static uint8_t big[3000], got[4000];
    uint8_t f[XBOX_NET_FRAME_MAX], r[XBOX_NET_FRAME_MAX], u[4096];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    uint16_t port;
    sock_t srv = open_server(&port);
    uint32_t len, i, total;
    int k;

    for (i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 7u + 3u);

    /* Out: the console's 3074 leaves this PC as 3074 (when free). */
    xbox_net_nat_send(n, f, ip_frame(f, k_mac, 17, XBOX_NAT_CONSOLE, LOOPBACK, 2, 0, 0, u,
                                     udp_payload(u, 3074, port, (const uint8_t *)"hello", 5)));
    k = (int)recvfrom(srv, (char *)got, sizeof got, 0, (struct sockaddr *)&from, &fl);
    CHECK(k == 5 && memcmp(got, "hello", 5) == 0, "the datagram did not arrive (%d)", k);
    CHECK(ntohs(from.sin_port) == 3074, "left from port %u, not 3074 (taken on this PC?)",
          ntohs(from.sin_port));

    /* Back: from where it was sent to, to the console's own address and port. */
    sendto(srv, "world!", 6, 0, (struct sockaddr *)&from, fl);
    len = next_frame(n, r);
    CHECK(len == 14 + 20 + 8 + 6, "reply frame length %u", len);
    if (len) {
        const uint8_t *ip = r + 14, *ud = ip + 20;
        CHECK(memcmp(r, k_mac, 6) == 0, "reply not to the console's Ethernet address");
        CHECK(get32(ip + 12) == LOOPBACK && get32(ip + 16) == XBOX_NAT_CONSOLE, "reply addresses");
        CHECK(get16(ud) == port && get16(ud + 2) == 3074, "reply ports %u -> %u",
              get16(ud), get16(ud + 2));
        CHECK(memcmp(ud + 8, "world!", 6) == 0, "reply payload");
        CHECK(udp_checksum_ok(ip, ud, 14), "reply UDP checksum");
    }

    /* A reply too big for a frame comes back in fragments. */
    sendto(srv, (const char *)big, sizeof big, 0, (struct sockaddr *)&from, fl);
    total = 0;
    for (k = 0; k < 4; k++) {
        const uint8_t *ip = r + 14;
        uint32_t off, plen;
        len = next_frame(n, r);
        if (!len)
            break;
        off = (get16(ip + 6) & 0x1FFFu) * 8u;
        plen = get16(ip + 2) - 20u;
        CHECK(fold(sum16(ip, 20, 0)) == 0, "fragment IP checksum");
        memcpy(got + off, ip + 20, plen);
        total += plen;
        if (!(get16(ip + 6) & 0x2000u))
            break;
    }
    CHECK(k == 2, "%d fragments, not 3", k + 1);
    CHECK(total == 8 + sizeof big && memcmp(got + 8, big, sizeof big) == 0,
          "fragments do not make the datagram (%u bytes)", total);

    /* The console's own fragments are put back together before they go out. */
    {
        uint32_t ulen = udp_payload(u, 3074, port, big, sizeof big);
        uint32_t off;
        for (off = 0; off < ulen; off += 1480) {
            uint32_t chunk = ulen - off > 1480 ? 1480 : ulen - off;
            xbox_net_nat_send(n, f, ip_frame(f, k_mac, 17, XBOX_NAT_CONSOLE, LOOPBACK, 3,
                                             off, off + chunk < ulen, u + off, chunk));
        }
        k = (int)recvfrom(srv, (char *)got, sizeof got, 0, (struct sockaddr *)&from, &fl);
        CHECK(k == (int)sizeof big && memcmp(got, big, sizeof big) == 0,
              "the console's fragments did not arrive whole (%d)", k);
    }

    /* Broadcasts stay behind the router. */
    xbox_net_nat_send(n, f, ip_frame(f, k_mac, 17, XBOX_NAT_CONSOLE, 0xFFFFFFFFu, 4, 0, 0, u,
                                     udp_payload(u, 3074, port, (const uint8_t *)"x", 1)));
#ifdef _WIN32
    closesocket(srv);
#else
    close(srv);
#endif
}

/* ---- TCP and ICMP ---- */

static void test_tcp_icmp(XboxNat *n)
{
    uint8_t f[XBOX_NET_FRAME_MAX], r[XBOX_NET_FRAME_MAX], t[20], e[16];
    uint32_t len;

    memset(t, 0, sizeof t);
    put16(t, 1025); put16(t + 2, 80); put32(t + 4, 1000); t[12] = 0x50; t[13] = 0x02;
    xbox_net_nat_send(n, f, ip_frame(f, k_mac, 6, XBOX_NAT_CONSOLE, LOOPBACK, 5, 0, 0, t, 20));
    len = next_frame(n, r);
    CHECK(len >= 54 && r[14 + 9] == 6 && (r[34 + 13] & 0x04), "no TCP reset for a SYN");
    CHECK(len >= 54 && get32(r + 34 + 8) == 1001, "the reset does not ack the SYN");

    memset(e, 0xAB, sizeof e);
    e[0] = 8; e[1] = 0; put16(e + 2, 0); put16(e + 2, fold(sum16(e, sizeof e, 0)));
    xbox_net_nat_send(n, f, ip_frame(f, k_mac, 1, XBOX_NAT_CONSOLE, XBOX_NAT_GATEWAY, 6, 0, 0,
                                     e, sizeof e));
    len = next_frame(n, r);
    CHECK(len == 14 + 20 + 16 && r[34] == 0 && fold(sum16(r + 34, 16, 0)) == 0,
          "no echo reply from the router");
}

int main(void)
{
    XboxNatConfig cfg;
    XboxNat *n;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    memset(&cfg, 0, sizeof cfg);
    cfg.quiet = 1;
    n = xbox_net_nat_open(&cfg, k_mac);
    if (!n) {
        printf("FAIL: the router did not start\n");
        return 1;
    }
    test_dhcp_and_arp(n);
    test_udp(n);
    test_tcp_icmp(n);
    xbox_net_nat_close(n);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("net_nat: all checks passed\n");
    return 0;
}
