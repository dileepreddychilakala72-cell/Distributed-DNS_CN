/*
 * server.c - distributed DNS server (primary or secondary role)
 *
 *   server -r primary|secondary [-p port] [-b bind_ip] -d dbfile
 *          [-f secondary_ip[:port]] [-l loss_pct] [-v]
 *
 * primary   : local lookup first; on a miss forwards to the secondary (-f)
 *             and relays the answer.
 * secondary : answers direct client queries AND forwarded queries; never
 *             forwards (no loops), so it works while the primary is down.
 *
 * Both echo the request ID.  Recent (client, request ID) -> response pairs
 * are cached so retransmissions get the same answer.
 * -l N simulates N% loss of incoming queries and of outgoing responses
 * (for local testing without netem).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "dns_protocol.h"

struct rec { char domain[DNS_DOMAIN_MAX]; char ip[DNS_IP_MAX]; };
static struct rec *db;
static int db_n;

#define CACHE_N   256
#define CACHE_TTL 30            /* seconds */
struct cache_ent {
    int used;
    struct in_addr addr;
    uint16_t port;
    uint32_t id;
    time_t t;
    struct dns_msg resp;
};
static struct cache_ent cache[CACHE_N];
static int cache_next;

static int is_primary = 1;
static int loss_pct;
static int verbose;
static struct sockaddr_in secondary_addr;
static int have_secondary;
static int fwd_sock = -1;
static const char *role_name = "primary";

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logf_(const char *fmt, ...)
{
    va_list ap;
    printf("[%s] ", role_name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int sim_drop(void) { return loss_pct > 0 && (rand() % 100) < loss_pct; }

static int load_db(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256], d[128], ip[128];
    int cap = 0;
    if (!f) { perror(path); return -1; }
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%127s %127s", d, ip) != 2) continue;
        if (strlen(d) >= DNS_DOMAIN_MAX || strlen(ip) >= DNS_IP_MAX) continue;
        if (db_n == cap) {
            cap = cap ? cap * 2 : 16;
            db = realloc(db, cap * sizeof(*db));
            if (!db) { perror("realloc"); exit(1); }
        }
        strcpy(db[db_n].domain, d);
        strcpy(db[db_n].ip, ip);
        db_n++;
    }
    fclose(f);
    return 0;
}

static const char *lookup(const char *name)
{
    for (int i = 0; i < db_n; i++)
        if (strcasecmp(db[i].domain, name) == 0) return db[i].ip;
    return NULL;
}

static struct cache_ent *cache_find(const struct sockaddr_in *c, uint32_t id)
{
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_N; i++) {
        struct cache_ent *e = &cache[i];
        if (!e->used) continue;
        if (now - e->t > CACHE_TTL) { e->used = 0; continue; }
        if (e->id == id && e->port == c->sin_port &&
            e->addr.s_addr == c->sin_addr.s_addr)
            return e;
    }
    return NULL;
}

static void cache_put(const struct sockaddr_in *c, const struct dns_msg *r)
{
    struct cache_ent *e = &cache[cache_next];
    cache_next = (cache_next + 1) % CACHE_N;
    e->used = 1; e->addr = c->sin_addr; e->port = c->sin_port;
    e->id = r->request_id; e->t = time(NULL); e->resp = *r;
}

static void send_msg(int sock, const struct dns_msg *m,
                     const struct sockaddr_in *to)
{
    uint8_t buf[DNS_WIRE_SIZE];
    dns_encode(m, buf);
    sendto(sock, buf, DNS_WIRE_SIZE, 0, (const struct sockaddr *)to, sizeof(*to));
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Ask the secondary.  Bounded: 2 attempts x 500 ms.  Returns status. */
static int forward_query(const struct dns_msg *q, char *ip_out)
{
    struct dns_msg f = *q, r;
    uint8_t buf[DNS_WIRE_SIZE + 8];
    f.flags |= DNS_FLAG_FORWARDED;
    for (int attempt = 0; attempt < 2; attempt++) {
        int64_t deadline = now_ms() + 500;
        send_msg(fwd_sock, &f, &secondary_addr);
        for (;;) {
            int64_t left = deadline - now_ms();
            struct pollfd p = { .fd = fwd_sock, .events = POLLIN };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
            ssize_t n = recv(fwd_sock, buf, sizeof(buf), 0);
            if (n < 0 || dns_decode(buf, (size_t)n, &r) < 0) continue;
            if (r.type != DNS_MSG_RESPONSE || r.request_id != q->request_id)
                continue;
            strcpy(ip_out, r.ip);
            return r.status;
        }
    }
    return DNS_ST_ERROR;
}

int main(int argc, char **argv)
{
    const char *dbfile = NULL, *bind_ip = NULL;
    uint16_t port = DNS_DEFAULT_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "r:p:b:d:f:l:v")) != -1) {
        switch (opt) {
        case 'r': is_primary = strcmp(optarg, "secondary") != 0;
                  role_name = is_primary ? "primary" : "secondary"; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'b': bind_ip = optarg; break;
        case 'd': dbfile = optarg; break;
        case 'f': if (dns_parse_hostport(optarg, &secondary_addr, DNS_DEFAULT_PORT)) {
                      fprintf(stderr, "bad -f address\n"); return 2; }
                  have_secondary = 1; break;
        case 'l': loss_pct = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s -r primary|secondary -d db [-p port] "
                    "[-b ip] [-f secondary_ip[:port]] [-l loss%%]\n", argv[0]);
            return 2;
        }
    }
    if (!dbfile) { fprintf(stderr, "-d dbfile required\n"); return 2; }
    if (load_db(dbfile) < 0) return 1;
    srand((unsigned)(time(NULL) ^ getpid()));

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind_ip && inet_pton(AF_INET, bind_ip, &me.sin_addr) != 1) {
        fprintf(stderr, "bad -b address\n"); return 2; }
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) { perror("bind"); return 1; }

    if (is_primary && have_secondary) {
        fwd_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (fwd_sock < 0) { perror("socket"); return 1; }
    }
    logf_("listening on %s:%u, %d records, forwarding %s, sim-loss %d%%",
          bind_ip ? bind_ip : "0.0.0.0", port, db_n,
          (is_primary && have_secondary) ? "enabled" : "disabled", loss_pct);

    for (;;) {
        uint8_t buf[512];
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        struct dns_msg q, r;
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
        if (n < 0) { if (errno == EINTR) continue; perror("recvfrom"); break; }
        if (dns_decode(buf, (size_t)n, &q) < 0) { logf_("dropped malformed datagram"); continue; }

        if (q.type == DNS_MSG_PING) {            /* dispatcher health probe */
            q.type = DNS_MSG_PONG;
            send_msg(sock, &q, &cli);
            continue;
        }
        if (q.type != DNS_MSG_QUERY) continue;
        if (sim_drop()) { logf_("SIM-LOSS dropped query id=%u", q.request_id); continue; }

        struct cache_ent *ce = cache_find(&cli, q.request_id);
        if (ce) {
            logf_("CACHE-HIT id=%u %s -> retransmitting same answer", q.request_id, q.domain);
            if (!sim_drop()) send_msg(sock, &ce->resp, &cli);
            continue;
        }

        memset(&r, 0, sizeof(r));
        r.version = DNS_PROTO_VERSION;
        r.type = DNS_MSG_RESPONSE;
        r.flags = q.flags;
        r.request_id = q.request_id;
        strcpy(r.domain, q.domain);

        const char *ip = lookup(q.domain);
        if (ip) {
            r.status = DNS_ST_OK;
            strcpy(r.ip, ip);
            logf_("id=%u %s -> %s (local)", q.request_id, q.domain, ip);
        } else if (is_primary && have_secondary && !(q.flags & DNS_FLAG_FORWARDED)) {
            logf_("id=%u %s not local, FORWARD to secondary", q.request_id, q.domain);
            r.status = (uint8_t)forward_query(&q, r.ip);
            logf_("id=%u %s secondary answered status=%d %s", q.request_id,
                  q.domain, r.status, r.status == DNS_ST_OK ? r.ip : "");
            if (r.status != DNS_ST_OK && r.status != DNS_ST_NOTFOUND) {
                r.status = DNS_ST_ERROR;
                r.ip[0] = '\0';
            }
        } else {
            r.status = DNS_ST_NOTFOUND;
            logf_("id=%u %s NOTFOUND", q.request_id, q.domain);
        }

        if (r.status != DNS_ST_ERROR) cache_put(&cli, &r);   /* never cache failures */
        if (!sim_drop()) send_msg(sock, &r, &cli);
        else logf_("SIM-LOSS dropped response id=%u", r.request_id);
    }
    (void)verbose;
    return 0;
}
