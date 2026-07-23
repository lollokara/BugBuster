// =============================================================================
// captive_dns.c — minimal fast-fail DNS responder (see .h).
//
// Raw lwIP UDP PCB on port 53. Every incoming query gets an NXDOMAIN reply
// with the original question section echoed back verbatim (most DNS clients
// expect that). No real record synthesis, no full name parsing -- just enough
// to find where the question section ends so we know how many bytes to copy.
// =============================================================================

#include "captive_dns.h"

#include <string.h>
#include "esp_log.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

static const char *TAG = "captive_dns";

static struct udp_pcb *s_pcb;

// DNS header is 12 bytes: ID(2) flags(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2).
#define DNS_HDR_LEN 12u

// Walk the QNAME labels (each [len byte][len bytes...], terminated by a
// zero-length label) starting at @p off, then skip QTYPE+QCLASS (4 bytes).
// Returns the offset just past the question section, or 0 if malformed/short.
static size_t question_end(const uint8_t *buf, size_t buf_len, size_t off)
{
    size_t i = off;
    // Bound the walk defensively -- untrusted network input.
    while (i < buf_len) {
        uint8_t label_len = buf[i];
        if (label_len == 0) {
            i += 1;               // the terminating zero-length label itself
            break;
        }
        // Reject compression pointers (top two bits set) -- shouldn't appear
        // in a query's own QNAME, and we don't need to follow them.
        if (label_len & 0xC0) return 0;
        i += 1 + label_len;
        if (i >= buf_len) return 0;
    }
    if (i + 4 > buf_len) return 0;   // QTYPE(2) + QCLASS(2)
    return i + 4;
}

static void dns_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    if (!p) return;

    // Coalesce into a small stack buffer; captive-portal probe queries are
    // tiny (well under 128 bytes). Anything bigger is not something we need
    // to answer usefully -- drop it.
    uint8_t buf[192];
    if (p->tot_len < DNS_HDR_LEN || p->tot_len > sizeof(buf)) {
        pbuf_free(p);
        return;
    }
    size_t len = pbuf_copy_partial(p, buf, p->tot_len, 0);
    pbuf_free(p);
    if (len < DNS_HDR_LEN) return;

    uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
    size_t qend;
    if (qdcount == 0) {
        qend = DNS_HDR_LEN;   // no question section to echo
    } else {
        qend = question_end(buf, len, DNS_HDR_LEN);
        if (qend == 0) return;   // malformed; just drop it
    }

    // Build the reply: header (same ID, QR=1/RCODE=3, same QDCOUNT,
    // ANCOUNT=NSCOUNT=ARCOUNT=0) + the question section copied verbatim.
    uint8_t reply[192];
    if (qend > sizeof(reply)) return;
    memcpy(reply, buf, DNS_HDR_LEN);
    reply[2] = 0x80 | (buf[2] & 0x01);   // QR=1, keep RD if the client set it
    reply[3] = 0x03;                     // RA=0, Z=0, RCODE=3 (NXDOMAIN)
    reply[6] = 0; reply[7] = 0;          // ANCOUNT=0
    reply[8] = 0; reply[9] = 0;          // NSCOUNT=0
    reply[10] = 0; reply[11] = 0;        // ARCOUNT=0
    if (qend > DNS_HDR_LEN) {
        memcpy(reply + DNS_HDR_LEN, buf + DNS_HDR_LEN, qend - DNS_HDR_LEN);
    }

    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)qend, PBUF_RAM);
    if (!out) return;
    memcpy(out->payload, reply, qend);
    udp_sendto(pcb, out, addr, port);
    pbuf_free(out);
}

esp_err_t captive_dns_start(void)
{
    if (s_pcb) return ESP_OK;

    LOCK_TCPIP_CORE();
    s_pcb = udp_new();
    if (!s_pcb) {
        UNLOCK_TCPIP_CORE();
        ESP_LOGE(TAG, "udp_new() failed");
        return ESP_ERR_NO_MEM;
    }
    err_t err = udp_bind(s_pcb, IP_ADDR_ANY, 53);
    if (err != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        UNLOCK_TCPIP_CORE();
        ESP_LOGE(TAG, "udp_bind(:53) failed: %d", (int)err);
        return ESP_FAIL;
    }
    udp_recv(s_pcb, dns_recv_cb, NULL);
    UNLOCK_TCPIP_CORE();

    ESP_LOGI(TAG, "fast-fail DNS responder up on :53");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_pcb) return;
    LOCK_TCPIP_CORE();
    udp_recv(s_pcb, NULL, NULL);
    udp_remove(s_pcb);
    s_pcb = NULL;
    UNLOCK_TCPIP_CORE();
    ESP_LOGI(TAG, "fast-fail DNS responder down");
}
