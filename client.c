/*
 * client.c - distributed DNS client with UDP reliability and failover
 *
 *   client [-d dispatcher[:port]] [-p primary[:port]] [-s secondary[:port]]
 *          [-t base_ms] [-r retries] [-D deadline_ms] [domain ...]
 *
 * Targets are tried in order: dispatcher (if given), primary, secondary.
 * Per target: up to `retries` attempts with timeout base, 2*base, 4*base
 * (exponential backoff, capped at 4*base) - defaults 1000/2000/4000 ms.
 * One overall deadline (default 15000 ms) bounds the whole lookup.
 *
 * A response is accepted only if it carries the outstanding request ID;
 * wrong/old/duplicate responses are discarded.
 *
 * Outcomes (and exit code): SUCCESS 0, NOTFOUND 1, TIMEOUT 2,
 * SERVER_UNAVAILABLE 3.
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

#define MAX_TARGETS 3

struct target { const char *name; struct sockaddr_in addr; char label[64]; };

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t gen_id(void)
{
    uint32_t id = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&id, sizeof(id), 1, f) != 1) id = 0; fclose(f); }
    if (!id) id = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ (uint32_t)now_ms();
    return id ? id : 1;
}

/* wait until a valid response for `id` arrives or `deadline` passes */
static int wait_response(int sock, uint32_t id, const char *domain,
                         int64_t deadline, struct dns_msg *resp)
{
    for (;;) {
        int64_t left = deadline - now_ms();
        struct pollfd p = { .fd = sock, .events = POLLIN };
        uint8_t buf[512];
        if (left <= 0) return 0;
        int pr = poll(&p, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return 0;
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) continue;
        if (dns_decode(buf, (size_t)n, resp) < 0) continue;
        if (resp->type != DNS_MSG_RESPONSE) continue;
        if (resp->request_id != id) {
            fprintf(stderr, "  (discarded response with old/wrong id %u)\n", resp->request_id);
            continue;
        }
        if (strcasecmp(resp->domain, domain) != 0) continue;
        return 1;
    }
}

enum { R_SUCCESS = 0, R_NOTFOUND = 1, R_TIMEOUT = 2, R_UNAVAILABLE = 3 };
static const char *names[] = { "SUCCESS", "NOTFOUND", "TIMEOUT", "SERVER_UNAVAILABLE" };

static int resolve(int sock, struct target *tg, int ntg, const char *domain,
                   int base_ms, int retries, int deadline_ms)
{
    struct dns_msg q, r;
    uint8_t buf[DNS_WIRE_SIZE];
    uint32_t id = gen_id();
    int64_t start = now_ms(), overall = start + deadline_ms;
    int attempts = 0, result = R_UNAVAILABLE, done = 0, served = -1;

    memset(&q, 0, sizeof(q));
    q.version = DNS_PROTO_VERSION;
    q.type = DNS_MSG_QUERY;
    q.request_id = id;
    strncpy(q.domain, domain, DNS_DOMAIN_MAX - 1);
    dns_encode(&q, buf);

    for (int t = 0; t < ntg && !done; t++) {
        for (int a = 0; a < retries && !done; a++) {
            int to = base_ms << (a > 2 ? 2 : a);          /* 1x, 2x, 4x, 4x ... */
            int64_t now = now_ms(), dl = now + to;
            if (now >= overall) { result = R_TIMEOUT; done = 2; break; }
            if (dl > overall) dl = overall;
            attempts++;
            fprintf(stderr, "attempt %d: %s (%s) id=%u timeout=%lld ms\n",
                    attempts, tg[t].name, tg[t].label, id, (long long)(dl - now));
            sendto(sock, buf, DNS_WIRE_SIZE, 0,
                   (struct sockaddr *)&tg[t].addr, sizeof(tg[t].addr));
            if (!wait_response(sock, id, domain, dl, &r)) continue;
            served = t;
            if (r.status == DNS_ST_OK) { result = R_SUCCESS; done = 1; }
            else if (r.status == DNS_ST_NOTFOUND) { result = R_NOTFOUND; done = 1; }
            else {
                fprintf(stderr, "  %s reported ERROR, trying next target\n", tg[t].name);
                break;
            }
        }
        if (done == 2) break;
        if (!done && t + 1 < ntg)
            fprintf(stderr, "%s unavailable, failing over to %s\n", tg[t].name, tg[t + 1].name);
    }
    if (!done && now_ms() >= overall) result = R_TIMEOUT;

    double lat = (double)(now_ms() - start);
    printf("Domain: %s\n", domain);
    printf("Result: %s\n", names[result]);
    if (result == R_SUCCESS) printf("IP Address: %s\n", r.ip);
    if (served >= 0 && (result == R_SUCCESS || result == R_NOTFOUND))
        printf("Served by: %s (%s)\n", tg[served].name, tg[served].label);
    printf("Attempts: %d\n", attempts);
    printf("DNS Lookup Latency: %.0f ms\n", lat);
    fflush(stdout);
    return result;
}

int main(int argc, char **argv)
{
    struct target tg[MAX_TARGETS];
    int ntg = 0, opt, base_ms = 1000, retries = 3, deadline_ms = 15000;
    const char *disp = NULL, *prim = "10.0.0.2", *sec = "10.0.0.3";

    while ((opt = getopt(argc, argv, "d:p:s:t:r:D:")) != -1) {
        switch (opt) {
        case 'd': disp = optarg; break;
        case 'p': prim = optarg; break;
        case 's': sec = optarg; break;
        case 't': base_ms = atoi(optarg); break;
        case 'r': retries = atoi(optarg); break;
        case 'D': deadline_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-d disp] [-p primary] [-s secondary] "
                    "[-t base_ms] [-r retries] [-D deadline_ms] [domain...]\n", argv[0]);
            return 2;
        }
    }
    if (base_ms < 1 || retries < 1) { fprintf(stderr, "bad -t/-r\n"); return 2; }

    if (disp) {
        tg[ntg].name = "dispatcher";
        if (dns_parse_hostport(disp, &tg[ntg].addr, DNS_DISPATCHER_PORT)) { fprintf(stderr, "bad -d\n"); return 2; }
        snprintf(tg[ntg].label, sizeof(tg[ntg].label), "%s", disp); ntg++;
    }
    tg[ntg].name = "primary";
    if (dns_parse_hostport(prim, &tg[ntg].addr, DNS_DEFAULT_PORT)) { fprintf(stderr, "bad -p\n"); return 2; }
    snprintf(tg[ntg].label, sizeof(tg[ntg].label), "%s", prim); ntg++;
    tg[ntg].name = "secondary";
    if (dns_parse_hostport(sec, &tg[ntg].addr, DNS_DEFAULT_PORT)) { fprintf(stderr, "bad -s\n"); return 2; }
    snprintf(tg[ntg].label, sizeof(tg[ntg].label), "%s", sec); ntg++;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int worst = 0;
    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            int r = resolve(sock, tg, ntg, argv[i], base_ms, retries, deadline_ms);
            if (r > worst) worst = r;
        }
    } else {
        char line[DNS_DOMAIN_MAX];
        for (;;) {
            printf("Enter domain name (empty to quit): "); fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0]) break;
            int r = resolve(sock, tg, ntg, line, base_ms, retries, deadline_ms);
            if (r > worst) worst = r;
        }
    }
    close(sock);
    return worst;
}
