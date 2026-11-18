/*
 * dns_protocol.h - shared wire protocol for client, primary, secondary
 * and dispatcher.  ALL components must include this one header.
 *
 * Wire format (88 bytes, fixed size, network byte order, no raw structs):
 *
 *   offset size field
 *   0      1    version      (DNS_PROTO_VERSION)
 *   1      1    message_type (QUERY/RESPONSE/PING/PONG)
 *   2      1    flags        (DNS_FLAG_FORWARDED ...)
 *   3      1    status       (OK/NOTFOUND/ERROR, meaningful in RESPONSE)
 *   4      4    request_id   (uint32, big endian)
 *   8      64   domain       (NUL-terminated string)
 *   72     16   ip_address   (NUL-terminated dotted-quad string)
 */
#ifndef DNS_PROTOCOL_H
#define DNS_PROTOCOL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DNS_PROTO_VERSION   1
#define DNS_DEFAULT_PORT    9090   /* primary / secondary servers   */
#define DNS_DISPATCHER_PORT 9000   /* optional dispatcher           */
#define DNS_DOMAIN_MAX      64
#define DNS_IP_MAX          16
#define DNS_WIRE_SIZE       88

enum dns_msg_type {
    DNS_MSG_QUERY    = 1,
    DNS_MSG_RESPONSE = 2,
    DNS_MSG_PING     = 3,   /* dispatcher health probe */
    DNS_MSG_PONG     = 4
};

enum dns_status {
    DNS_ST_OK       = 0,
    DNS_ST_NOTFOUND = 1,   /* lookup completed, domain does not exist   */
    DNS_ST_ERROR    = 2    /* server could not complete the lookup      */
};

/* Set by the primary when it forwards to the secondary: the secondary
 * must never forward again (prevents forwarding loops). */
#define DNS_FLAG_FORWARDED 0x01

struct dns_msg {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  status;
    uint32_t request_id;
    char     domain[DNS_DOMAIN_MAX];
    char     ip[DNS_IP_MAX];
};

static inline size_t dns_encode(const struct dns_msg *m, uint8_t *buf)
{
    uint32_t id = htonl(m->request_id);
    memset(buf, 0, DNS_WIRE_SIZE);
    buf[0] = m->version;
    buf[1] = m->type;
    buf[2] = m->flags;
    buf[3] = m->status;
    memcpy(buf + 4, &id, 4);
    memcpy(buf + 8, m->domain, DNS_DOMAIN_MAX);
    memcpy(buf + 72, m->ip, DNS_IP_MAX);
    return DNS_WIRE_SIZE;
}

/* returns 0 on success, -1 if the datagram is not a valid message */
static inline int dns_decode(const uint8_t *buf, size_t len, struct dns_msg *m)
{
    uint32_t id;
    if (len != DNS_WIRE_SIZE) return -1;
    if (buf[0] != DNS_PROTO_VERSION) return -1;
    if (buf[1] < DNS_MSG_QUERY || buf[1] > DNS_MSG_PONG) return -1;
    m->version = buf[0];
    m->type    = buf[1];
    m->flags   = buf[2];
    m->status  = buf[3];
    memcpy(&id, buf + 4, 4);
    m->request_id = ntohl(id);
    memcpy(m->domain, buf + 8, DNS_DOMAIN_MAX);
    memcpy(m->ip, buf + 72, DNS_IP_MAX);
    m->domain[DNS_DOMAIN_MAX - 1] = '\0';
    m->ip[DNS_IP_MAX - 1] = '\0';
    return 0;
}

/* Parse "a.b.c.d" or "a.b.c.d:port" into a sockaddr_in. 0 = ok. */
static inline int dns_parse_hostport(const char *s, struct sockaddr_in *a,
                                     uint16_t defport)
{
    char host[64];
    const char *colon = strchr(s, ':');
    size_t hl = colon ? (size_t)(colon - s) : strlen(s);
    if (hl == 0 || hl >= sizeof(host)) return -1;
    memcpy(host, s, hl);
    host[hl] = '\0';
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(colon ? (uint16_t)atoi(colon + 1) : defport);
    return inet_pton(AF_INET, host, &a->sin_addr) == 1 ? 0 : -1;
}

#endif
