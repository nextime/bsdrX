/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
/* quest_sim — pretend to be a Quest headset and connect to a REAL Bigscreen
 * Remote Desktop PC host (e.g. on a Windows box on the LAN). We run the headset
 * side of the protocol: discover -> pair -> /start, beacon 0x01234567 on 45002,
 * then DTLS-client on 45004. Because WE are a DTLS endpoint we hold the keys, so
 * we log in PLAINTEXT exactly what the host sends: which port carries media,
 * SRTP vs plain, the H.264 NAL structure (dumped to disk), and the decrypted
 * 45004 data-channel bytes (SCTP? raw? who drives it). This is the ground truth
 * the reversed host can't give us.
 *
 *   quest_sim <host-ip> [seconds]
 *
 * Dumps any decoded video to quest_sim_<port>.h264 for ffprobe.
 */
#include "bsdr/platform.h"
#include "bsdr/net.h"
#include "bsdr/protocol.h"
#include "bsdr/json.h"
#include "bsdr/udp_transport.h"
#include "bsdr/dtls.h"
#include "bsdr/srtp_util.h"
#include "bsdr/video.h"
#include "bsdr/log.h"

#include <srtp2/srtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

/* ---- discovery + HTTP control (Quest side), adapted from test_client.c ---- */
static bool discover(const char *ip, char *code, size_t codelen) {
    bsdr_socket_t s = bsdr_udp_bind("0.0.0.0", BSDR_REMOTE_CLIENT_INFO_PORT, true);
    if (s == BSDR_INVALID_SOCKET) { printf("bind 45001 failed (agent still running?)\n"); return false; }
    struct sockaddr_in to; bsdr_sockaddr_make(&to, ip, BSDR_DISCOVERY_REQUEST_PORT);
    for (int try = 0; try < 10; try++) {
        bsdr_udp_sendto(s, BSDR_BROADCAST_HEADER, BSDR_BROADCAST_HEADER_LEN, &to);
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        struct timeval tv = { 1, 0 };
        if (select((int)s + 1, &rf, NULL, NULL, &tv) > 0) {
            uint8_t buf[1024]; struct sockaddr_in from;
            int n = bsdr_udp_recvfrom(s, buf, sizeof(buf) - 1, &from);
            if (n > BSDR_BROADCAST_HEADER_LEN) {
                buf[n] = '\0';
                const char *json = (const char *)buf + BSDR_BROADCAST_HEADER_LEN;
                printf("discovered host: %s\n", json);
                bool ok = bsdr_json_get_str(json, "pairingRequestCode", code, codelen);
                bsdr_socket_close(s); return ok;
            }
        }
        printf("  ...no reply (try %d/10), resending discovery\n", try + 1);
    }
    bsdr_socket_close(s);
    printf("no discovery reply from %s — pass the 6-digit code as arg 3 to skip discovery\n", ip);
    return false;
}

static bool http(const char *ip, const char *method, const char *path,
                 const char *body, char *out, size_t outlen) {
    bsdr_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in to; bsdr_sockaddr_make(&to, ip, BSDR_HTTP_SERVER_PORT);
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
        printf("connect %s:45678 failed\n", ip); bsdr_socket_close(s); return false;
    }
    char req[1024]; int blen = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        method, path, ip, blen, body ? body : "");
    bsdr_send_all(s, req, (size_t)n);
    char resp[2048]; int total = 0, r;
    while ((r = (int)recv(s, resp + total, (int)(sizeof(resp) - 1 - total), 0)) > 0) {
        total += r; if (total >= (int)sizeof(resp) - 1) break;
    }
    bsdr_socket_close(s); resp[total > 0 ? total : 0] = '\0';
    char *eol = strstr(resp, "\r\n"); char *bs = strstr(resp, "\r\n\r\n");
    if (eol) *eol = '\0';
    printf("%s %s -> %s\n", method, path, resp);
    if (bs && out) snprintf(out, outlen, "%s", bs + 4);
    return bs != NULL;
}

/* ---- per-port classification + media decode ---- */
typedef struct {
    int port;
    unsigned dtls, srtp, beacon, keepalive, other, unprotect_fail;
    srtp_t srtp_ctx; int have_srtp;
    bsdr_dtls *dtls_obj;       /* completed handshake for this port (server/client) */
    bsdr_h264_depay *depay;
    FILE *dump; char nal_seen[64]; int nal_logged;
} port_state;

static void note_nal_types(port_state *ps, const uint8_t *au, size_t len) {
    if (ps->nal_logged) return;
    char out[128]; out[0] = 0;
    for (size_t i = 0; i + 4 < len && strlen(out) < 100; i++)
        if (au[i] == 0 && au[i+1] == 0 &&
            ((au[i+2] == 1) || (au[i+2] == 0 && au[i+3] == 1))) {
            size_t h = au[i+2] == 1 ? i + 3 : i + 4;
            char b[8]; snprintf(b, sizeof(b), "%d ", au[h] & 0x1f); strcat(out, b);
        }
    printf("  [port %d] FIRST video NAL types: %s (7=SPS 8=PPS 5=IDR 1=P 6=SEI 12=FILLER)\n",
           ps->port, out);
    ps->nal_logged = 1;
}

static void handle_rtp(port_state *ps, uint8_t *buf, int n) {
    int len = n;
    if (ps->have_srtp) {
        if (srtp_unprotect(ps->srtp_ctx, buf, &len) != srtp_err_status_ok) {
            ps->unprotect_fail++;
            if (ps->unprotect_fail == 1)
                printf("  [port %d] SRTP unprotect FAILED (keys not for this port, or PLAIN rtp?)\n",
                       ps->port);
            return;   /* maybe plain RTP — caller already logged header */
        }
    }
    ps->srtp++;
    if (len < 12) return;
    int pt = buf[1] & 0x7f;
    if (!ps->nal_logged)
        printf("  [port %d] RTP pt=%d ssrc=%08x seq=%u (decrypted %dB)\n", ps->port, pt,
               (buf[8]<<24)|(buf[9]<<16)|(buf[10]<<8)|buf[11],
               (buf[2]<<8)|buf[3], len);
    if (pt == BSDR_AUDIO_PT_DEFAULT) return;   /* Opus; just count */
    static uint8_t out[1024*1024]; size_t outlen = 0;
    bsdr_h264_depay_feed(ps->depay, buf + 12, (size_t)(len - 12), out, sizeof(out), &outlen);
    if (outlen) { note_nal_types(ps, out, outlen); if (ps->dump) fwrite(out, 1, outlen, ps->dump); }
}

static void dc_appdata(const uint8_t *d, int n, void *u) {
    port_state *ps = (port_state *)u;
    printf("  [port %d DTLS app-data plaintext] %dB:", ps ? ps->port : 0, n);
    for (int i = 0; i < n && i < 48; i++) printf(" %02x", d[i]);
    /* SCTP-over-DTLS? RFC 8261 packet = 12B common hdr then chunks; chunk type at [12] */
    if (n >= 13) printf("   (if SCTP: chunk_type=%u  1=INIT 2=INIT_ACK 6=ABORT 0=DATA)", d[12]);
    printf("\n");
}

typedef struct { bsdr_udp *udp; volatile int stop; } beacon_ctx;
static void beacon_loop(void *arg) {
    beacon_ctx *b = arg;
    const uint8_t magic[4] = { 0x67, 0x45, 0x23, 0x01 };   /* 0x01234567 LE */
    while (!b->stop) { bsdr_udp_send(b->udp, magic, 4); bsdr_sleep_ms(1000); }
}

static void classify(port_state *ps, uint8_t *buf, int n) {
    if (n <= 0) return;
    uint8_t b = buf[0];
    if (b >= 20 && b < 64) {                      /* DTLS record */
        ps->dtls++;
        if (ps->dtls_obj)                         /* decrypt app-data (data channel / control) */
            bsdr_dtls_process(ps->dtls_obj, buf, n, dc_appdata, ps);
        else if (ps->dtls == 1)
            printf("  [port %d] *** DTLS record (no completed handshake on this port) ***\n", ps->port);
    } else if (b >= 128 && b < 192) {             /* RTP/SRTP */
        handle_rtp(ps, buf, n);
    } else if (n == 4 && b == 0x67) {             /* 0x01234567 beacon */
        ps->beacon++;
    } else if (n == 1) {                          /* 0x00 keepalive */
        ps->keepalive++;
    } else {
        ps->other++;
        if (ps->other <= 3) {
            printf("  [port %d] OTHER %dB:", ps->port, n);
            for (int i = 0; i < n && i < 16; i++) printf(" %02x", buf[i]);
            printf("\n");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <host-ip> [seconds] [pairing-code]\n", argv[0]); return 1; }
    const char *ip = argv[1];
    int secs = argc > 2 ? atoi(argv[2]) : 15;
    bsdr_log_set_level(BSDR_LOG_INFO);
    if (!bsdr_platform_init()) return 1;
    bsdr_srtp_global_init();

    char code[16] = {0};
    if (argc > 3) {              /* manual pairing code: skip discovery */
        snprintf(code, sizeof(code), "%s", argv[3]);
        printf("using manual pairing code: %s\n", code);
    } else if (!discover(ip, code, sizeof(code))) {
        return 1;
    }
    printf("pairing code: %s\n", code);
    char body[256], resp[512];
    snprintf(body, sizeof(body),
        "{\"pairingRequestCode\":\"%s\",\"deviceId\":\"quest-sim-001\","
        "\"deviceName\":\"QuestSim\"}", code);
    if (!http(ip, "POST", "/pair", body, resp, sizeof(resp))) return 1;
    char pid[128] = {0};
    if (!bsdr_json_get_str(resp, "pairingId", pid, sizeof(pid))) {
        printf("no pairingId in: %s\n", resp); return 1;
    }
    char path[256];

    /* open the three media sockets (local port == remote port, like the Quest) */
    bsdr_udp u2, u3, u4;
    bsdr_udp_open(&u2, BSDR_REMOTE_DESKTOP_PORT, ip, BSDR_REMOTE_DESKTOP_PORT);
    bsdr_udp_open(&u3, BSDR_REMOTE_DESKTOP_PORT+1, ip, BSDR_REMOTE_DESKTOP_PORT+1);
    bsdr_udp_open(&u4, BSDR_REMOTE_DATA_PORT, ip, BSDR_REMOTE_DATA_PORT);

    /* beacon on 45002 so the host advances past "waiting for headset" */
    beacon_ctx bc = { &u2, 0 };
    bsdr_thread *bt = bsdr_thread_start(beacon_loop, &bc);

    /* tell the host to start streaming to us */
    snprintf(path, sizeof(path), "/start/%s", pid); http(ip, "GET", path, NULL, NULL, 0);

    port_state p2 = { .port = BSDR_REMOTE_DESKTOP_PORT };
    port_state p3 = { .port = BSDR_REMOTE_DESKTOP_PORT+1 };
    port_state p4 = { .port = BSDR_REMOTE_DATA_PORT };
    p2.depay = bsdr_h264_depay_new();
    p3.depay = bsdr_h264_depay_new();
    p4.depay = bsdr_h264_depay_new();
    p2.dump = fopen("quest_sim_45002.h264", "wb");
    p3.dump = fopen("quest_sim_45003_audio.bin", "wb");
    p4.dump = fopen("quest_sim_45004.h264", "wb");

    /* 45004: we are the DTLS *client* (host listens as server post-/start). */
    printf("\n=== 45004 DTLS client handshake (data/input channel) ===\n");
    p4.dtls_obj = bsdr_dtls_new(&u4, BSDR_DTLS_CLIENT);
    printf("45004 handshake: %s\n",
           (p4.dtls_obj && bsdr_dtls_handshake(p4.dtls_obj, 6000, NULL)) ? "OK" : "FAILED");

    /* 45002 (video) + 45003 (audio): the HOST is the DTLS *client* and connects to
     * US — so the Quest is the DTLS SERVER here. Complete those handshakes and key
     * SRTP from each port's own handshake (recv_master = the host/sender's key). */
    for (int i = 0; i < 2; i++) {
        port_state *p = i ? &p3 : &p2;
        bsdr_udp *u = i ? &u3 : &u2;
        printf("\n=== %d DTLS server handshake (host = client; media channel) ===\n", p->port);
        p->dtls_obj = bsdr_dtls_new(u, BSDR_DTLS_SERVER);
        if (p->dtls_obj && bsdr_dtls_handshake(p->dtls_obj, 6000, NULL)) {
            char prof[64] = {0}, subj[128] = {0};
            bsdr_dtls_peer_info(p->dtls_obj, subj, sizeof(subj), prof, sizeof(prof));
            printf("%d handshake: OK (peer=%s srtp=%s)\n", p->port,
                   subj[0] ? subj : "?", prof[0] ? prof : "(defaulted)");
            bsdr_srtp_keys k;
            if (bsdr_dtls_export_srtp(p->dtls_obj, &k))
                p->have_srtp = bsdr_srtp_session_create(&p->srtp_ctx, k.recv_master,
                    k.profile ? k.profile : BSDR_SRTP_AES128_CM_SHA1_80, 0, true);
        } else {
            printf("%d handshake: FAILED/timeout (host didn't initiate here?)\n", p->port);
        }
    }

    printf("\n=== listening %ds — logging what the real host sends ===\n", secs);
    uint64_t t0 = bsdr_now_ms(), last_hb = 0;
    uint8_t buf[4096];
    int maxfd = (int)u4.sock;
    if ((int)u2.sock > maxfd) maxfd = (int)u2.sock;
    if ((int)u3.sock > maxfd) maxfd = (int)u3.sock;
    while ((int)((bsdr_now_ms() - t0) / 1000) < secs) {
        uint64_t now = bsdr_now_ms();
        if (now - last_hb >= 3000) {     /* keep the pairing alive */
            snprintf(path, sizeof(path), "/heartbeat/%s", pid);
            http(ip, "GET", path, NULL, NULL, 0); last_hb = now;
        }
        fd_set rf; FD_ZERO(&rf);
        FD_SET(u2.sock, &rf); FD_SET(u3.sock, &rf); FD_SET(u4.sock, &rf);
        struct timeval tv = { 0, 200000 };
        if (select(maxfd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        if (FD_ISSET(u2.sock, &rf)) { int n = bsdr_udp_recv(&u2, buf, sizeof(buf), 0); if (n>0) classify(&p2, buf, n); }
        if (FD_ISSET(u3.sock, &rf)) { int n = bsdr_udp_recv(&u3, buf, sizeof(buf), 0); if (n>0) classify(&p3, buf, n); }
        if (FD_ISSET(u4.sock, &rf)) { int n = bsdr_udp_recv(&u4, buf, sizeof(buf), 0); if (n>0) classify(&p4, buf, n); }
    }

    bc.stop = 1; bsdr_thread_join(bt);
    snprintf(path, sizeof(path), "/stop/%s", pid);   http(ip, "GET", path, NULL, NULL, 0);
    snprintf(path, sizeof(path), "/unpair/%s", pid); http(ip, "GET", path, NULL, NULL, 0);

    printf("\n=============== SUMMARY (what the real host sent) ===============\n");
    port_state *all[3] = { &p2, &p3, &p4 };
    for (int i = 0; i < 3; i++) {
        port_state *p = all[i];
        printf("port %d: dtls=%u srtp=%u beacon=%u keepalive=%u other=%u unprotect_fail=%u\n",
               p->port, p->dtls, p->srtp, p->beacon, p->keepalive, p->other, p->unprotect_fail);
    }
    printf("\nVIDEO PORT = whichever has srtp>0 + NAL types logged above.\n");
    printf("DATA  PORT 45004: dtls records above; DATACHANNEL lines show plaintext (SCTP vs raw).\n");
    printf("dumped video -> quest_sim_45002.h264 / quest_sim_45004.h264 (ffprobe them).\n");

    if (p2.dump) fclose(p2.dump);
    if (p4.dump) fclose(p4.dump);
    bsdr_platform_cleanup();
    return 0;
}
