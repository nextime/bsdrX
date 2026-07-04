/* Make bsdr_sctp emit a real SCTP INIT (raw SCTP-over-UDP, cloud path) to a loopback listener,
 * and dump/parse the bytes — to verify our INIT is well-formed with csum=0, matching BigSoup. */
#include "bsdr/sctp.h"
#include "bsdr/udp_transport.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static unsigned be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char *p) { return ((unsigned)p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static void dump(const unsigned char *p, int n) {
    printf("=== bsdrX SCTP packet (%d bytes) ===\n", n);
    for (int i = 0; i < n; i++) { printf("%02x ", p[i]); if (i % 16 == 15) printf("\n"); }
    printf("\n");
    if (n < 12) return;
    printf("SCTP hdr: src_port=%u dst_port=%u vtag=%08x checksum=%02x%02x%02x%02x %s\n",
           be16(p), be16(p + 2), be32(p + 4), p[8], p[9], p[10], p[11],
           (p[8]|p[9]|p[10]|p[11]) ? "(NON-ZERO!)" : "(csum=0, matches BigSoup)");
    int off = 12;
    while (off + 4 <= n) {
        unsigned ctype = p[off], clen = be16(p + off + 2);
        printf("  chunk type=%u flags=%u len=%u\n", ctype, p[off + 1], clen);
        if (ctype == 1 && off + 20 <= n) {   /* INIT */
            printf("  INIT: init_tag=%08x a_rwnd=%u OS=%u MIS=%u init_tsn=%08x\n",
                   be32(p+off+4), be32(p+off+8), be16(p+off+12), be16(p+off+14), be32(p+off+16));
            int po = off + 20;               /* INIT optional params */
            while (po + 4 <= off + (int)clen && po + 4 <= n) {
                unsigned pt = be16(p+po), pl = be16(p+po+2);
                printf("    param type=0x%04x len=%u\n", pt, pl);
                if (pl < 4) break;
                po += (pl + 3) & ~3;
            }
        }
        if (clen < 4) break;
        off += (clen + 3) & ~3;
    }
}

int main(void) {
    bsdr_log_set_level(BSDR_LOG_DEBUG);

    int ls = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(39777); a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
    struct timeval tv = { 1, 0 };
    setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    bsdr_udp udp;
    if (!bsdr_udp_open(&udp, 0, "127.0.0.1", 39777)) { printf("udp open failed\n"); return 1; }
    bsdr_sctp *s = bsdr_sctp_new_udp(&udp, true, NULL, NULL);
    if (!s) { printf("sctp_new_udp failed\n"); return 1; }
    if (!bsdr_sctp_start(s, 5000)) { printf("sctp_start failed\n"); }

    unsigned char buf[2048];
    for (int i = 0; i < 10; i++) {
        int n = recv(ls, buf, sizeof buf, 0);
        if (n > 0) { dump(buf, n); break; }
        bsdr_sctp_handle_timers(200);
        usleep(150000);
    }
    bsdr_sctp_free(s);
    bsdr_udp_close(&udp);
    return 0;
}
