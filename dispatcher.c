/*
 * dispatcher.c - optional round-robin load balancer with health checks
 *
 *   dispatcher [-p listen_port] [-b bind_ip] [-i probe_ms] [-m max_missed]
 *              backend1[:port] backend2[:port] ...
 *
 * Clients send normal QUERY messages to the dispatcher.  It forwards each
 * query to the next HEALTHY backend (round-robin) and relays the response
 * to the client.  Every probe interval it sends PING to every backend; a
 * backend that misses `max_missed` consecutive probes is removed from
 * rotation until it answers a probe again.
 * Every backend must be able to answer independently (the secondary holds
 * a replica; the primary falls back to forwarding to the secondary).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "dns_protocol.h"

#define MAX_BACKENDS 8
#define MAX_PENDING  256

struct backend {
    struct sockaddr_in addr;
    char label[48];
    int healthy, missed, awaiting;
    uint32_t probe_id;
    unsigned long served;
};
struct pend { int used; uint32_t id; struct sockaddr_in client; int64_t t; };

static struct backend be[MAX_BACKENDS];
static int nbe, rr_next;
static struct pend pend[MAX_PENDING];

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    printf("[dispatcher] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void send_msg(int sock, const struct dns_msg *m, const struct sockaddr_in *to)
{
    uint8_t buf[DNS_WIRE_SIZE];
    dns_encode(m, buf);
    sendto(sock, buf, DNS_WIRE_SIZE, 0, (const struct sockaddr *)to, sizeof(*to));
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int find_backend(const struct sockaddr_in *a)
{
    for (int i = 0; i < nbe; i++) if (same_addr(&be[i].addr, a)) return i;
    return -1;
}

static void mark_alive(int i)
{
    be[i].missed = 0;
    be[i].awaiting = 0;
    if (!be[i].healthy) { be[i].healthy = 1; logf_("backend %s is UP (back in rotation)", be[i].label); }
}

static int pick_backend(void)
{
    for (int k = 0; k < nbe; k++) {
        int i = (rr_next + k) % nbe;
        if (be[i].healthy) { rr_next = (i + 1) % nbe; return i; }
    }
    return -1;
}

int main(int argc, char **argv)
{
    uint16_t port = DNS_DISPATCHER_PORT;
    const char *bind_ip = NULL;
    int probe_ms = 1000, max_missed = 3, opt;

    while ((opt = getopt(argc, argv, "p:b:i:m:")) != -1) {
        switch (opt) {
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'b': bind_ip = optarg; break;
        case 'i': probe_ms = atoi(optarg); break;
        case 'm': max_missed = atoi(optarg); break;
        default: fprintf(stderr, "usage: %s [-p port] [-b ip] [-i ms] [-m n] backend...\n", argv[0]); return 2;
        }
    }
    for (int i = optind; i < argc && nbe < MAX_BACKENDS; i++) {
        if (dns_parse_hostport(argv[i], &be[nbe].addr, DNS_DEFAULT_PORT)) {
            fprintf(stderr, "bad backend %s\n", argv[i]); return 2; }
        snprintf(be[nbe].label, sizeof(be[nbe].label), "%s", argv[i]);
        be[nbe].healthy = 1;                       /* optimistic start */
        nbe++;
    }
    if (nbe == 0) { fprintf(stderr, "need at least one backend\n"); return 2; }

    int lsock = socket(AF_INET, SOCK_DGRAM, 0), usock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind_ip && inet_pton(AF_INET, bind_ip, &me.sin_addr) != 1) { fprintf(stderr, "bad -b\n"); return 2; }
    if (lsock < 0 || usock < 0 || bind(lsock, (struct sockaddr *)&me, sizeof(me)) < 0) { perror("socket/bind"); return 1; }
    srand((unsigned)(time(NULL) ^ getpid()));
    logf_("listening on port %u, %d backends, probe %d ms, down after %d missed",
          port, nbe, probe_ms, max_missed);

    int64_t next_probe = now_ms() + probe_ms;
    for (;;) {
        struct pollfd p[2] = { { lsock, POLLIN, 0 }, { usock, POLLIN, 0 } };
        int64_t wait = next_probe - now_ms();
        if (wait < 0) wait = 0;
        if (poll(p, 2, (int)wait) < 0 && errno != EINTR) { perror("poll"); break; }

        if (now_ms() >= next_probe) {                      /* health probes */
            next_probe += probe_ms;
            for (int i = 0; i < nbe; i++) {
                if (be[i].awaiting && ++be[i].missed >= max_missed && be[i].healthy) {
                    be[i].healthy = 0;
                    logf_("backend %s is DOWN after %d missed probes (removed from rotation)",
                          be[i].label, be[i].missed);
                }
                struct dns_msg ping;
                memset(&ping, 0, sizeof(ping));
                ping.version = DNS_PROTO_VERSION;
                ping.type = DNS_MSG_PING;
                ping.request_id = be[i].probe_id = (uint32_t)rand() | 1;
                be[i].awaiting = 1;
                send_msg(usock, &ping, &be[i].addr);
            }
        }

        if (p[1].revents & POLLIN) {                       /* from backends */
            uint8_t buf[512];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            struct dns_msg m;
            ssize_t n = recvfrom(usock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            int b = (n > 0 && dns_decode(buf, (size_t)n, &m) == 0) ? find_backend(&from) : -1;
            if (b >= 0) {
                if (m.type == DNS_MSG_PONG && m.request_id == be[b].probe_id) mark_alive(b);
                else if (m.type == DNS_MSG_RESPONSE) {
                    mark_alive(b);
                    for (int k = 0; k < MAX_PENDING; k++)
                        if (pend[k].used && pend[k].id == m.request_id) {
                            send_msg(lsock, &m, &pend[k].client);
                            pend[k].used = 0;
                            break;
                        }
                }
            }
        }

        if (p[0].revents & POLLIN) {                       /* from clients */
            uint8_t buf[512];
            struct sockaddr_in cli;
            socklen_t cl = sizeof(cli);
            struct dns_msg q;
            ssize_t n = recvfrom(lsock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
            if (n > 0 && dns_decode(buf, (size_t)n, &q) == 0 && q.type == DNS_MSG_QUERY) {
                int b = pick_backend();
                if (b < 0) {
                    q.type = DNS_MSG_RESPONSE; q.status = DNS_ST_ERROR; q.ip[0] = '\0';
                    send_msg(lsock, &q, &cli);
                    logf_("id=%u %s: NO healthy backend", q.request_id, q.domain);
                    continue;
                }
                int slot = -1;
                for (int k = 0; k < MAX_PENDING && slot < 0; k++)
                    if (pend[k].used && pend[k].id == q.request_id) slot = k;      /* retransmit */
                for (int k = 0; k < MAX_PENDING && slot < 0; k++) if (!pend[k].used) slot = k;
                if (slot < 0) {                                                    /* evict oldest */
                    slot = 0;
                    for (int k = 1; k < MAX_PENDING; k++) if (pend[k].t < pend[slot].t) slot = k;
                }
                pend[slot] = (struct pend){ 1, q.request_id, cli, now_ms() };
                be[b].served++;
                logf_("DISPATCH id=%u %s -> %s (served by this backend: %lu)",
                      q.request_id, q.domain, be[b].label, be[b].served);
                send_msg(usock, &q, &be[b].addr);
            }
        }
    }
    return 0;
}
