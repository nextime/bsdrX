/* Offline validation of the owner-mic sniffer's packet path (no root / no NIC needed).
 * Includes the production source directly so it exercises the REAL handle_ip() parsing:
 * IP/UDP/RTP header walk, 8-byte [ssrc][frame_id] trailer strip, 2x-send dedupe, mono
 * Opus flow-lock, and private-dst / wrong-src rejection. Packets are built with real
 * libopus output so the decode path is genuine. */
#define BSDR_MICSNIFF_TEST 1
#include "../src/micsniff.c"

#include <assert.h>

/* build [IP][UDP][RTP][opus][8B trailer]; returns total length */
static int build_pkt(uint8_t *buf, uint32_t src_be, uint32_t dst_be, uint16_t dport,
                     uint32_t ssrc, const uint8_t *opus, int olen, uint32_t frame_id) {
    int rtp = 12 + olen + 8;
    int ulen = 8 + rtp;
    int tot = 20 + ulen;
    memset(buf, 0, tot);
    buf[0] = 0x45; buf[2] = (uint8_t)(tot >> 8); buf[3] = (uint8_t)tot;
    buf[9] = 17;                                  /* UDP */
    memcpy(buf + 12, &src_be, 4);
    memcpy(buf + 16, &dst_be, 4);
    uint8_t *u = buf + 20;
    u[0] = 0x30; u[1] = 0x39;                      /* sport 12345 */
    u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
    u[4] = (uint8_t)(ulen >> 8); u[5] = (uint8_t)ulen;
    uint8_t *r = u + 8;
    r[0] = 0x80; r[1] = 100;                        /* v2, pt 100 */
    r[8] = (uint8_t)(ssrc >> 24); r[9] = (uint8_t)(ssrc >> 16);
    r[10] = (uint8_t)(ssrc >> 8); r[11] = (uint8_t)ssrc;
    memcpy(r + 12, opus, olen);
    uint8_t *tr = r + 12 + olen;                    /* [u32 ssrc LE][u32 frame_id LE] */
    tr[0]=(uint8_t)ssrc; tr[1]=(uint8_t)(ssrc>>8); tr[2]=(uint8_t)(ssrc>>16); tr[3]=(uint8_t)(ssrc>>24);
    tr[4]=(uint8_t)frame_id; tr[5]=(uint8_t)(frame_id>>8); tr[6]=(uint8_t)(frame_id>>16); tr[7]=(uint8_t)(frame_id>>24);
    return tot;
}

static int encode_tone(OpusEncoder *e, int channels, uint8_t *out) {
    int16_t pcm[480 * 2];                           /* integer sawtooth — no libm needed */
    for (int i = 0; i < 480; i++) {
        int16_t s = (int16_t)(((i * 137) % 16000) - 8000);
        pcm[i * channels] = s;
        if (channels == 2) pcm[i * channels + 1] = s;
    }
    return opus_encode(e, pcm, 480, out, 4000);
}

int main(void) {
    bsdr_log_set_level(BSDR_LOG_WARN);
    int err = 0;
    OpusEncoder *monoenc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err); assert(monoenc && !err);
    OpusEncoder *stereoenc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err); assert(stereoenc && !err);

    struct bsdr_micsniff s;
    memset(&s, 0, sizeof s);
    s.player = NULL;                                /* handle_ip skips playout when NULL */
    s.dec = opus_decoder_create(48000, 1, &err); assert(s.dec && !err);
    snprintf(s.quest_ip, sizeof s.quest_ip, "192.168.1.109");
    s.quest_be = inet_addr("192.168.1.109");

    uint32_t cloud = inet_addr("203.0.113.10");    /* public (example non-LAN address) */
    uint32_t lan   = inet_addr("192.168.1.114");   /* the LAN host — must be ignored */
    uint8_t opus[4000]; uint8_t pkt[2048];

    /* 1) a mono Opus packet from the Quest to the cloud locks the flow and decodes */
    int olen = encode_tone(monoenc, 1, opus); assert(olen > 0);
    assert((opus[0] & 0x04) == 0);                  /* mono TOC */
    int n = build_pkt(pkt, s.quest_be, cloud, 40000, 0xAABBCCDD, opus, olen, 1000);
    handle_ip(&s, pkt, n);
    assert(s.have_flow == 1);
    assert(s.flow_ssrc == 0xAABBCCDD);
    assert(s.decoded == 1);

    /* 2) the 2x-duplicate (same frame_id) is dropped */
    handle_ip(&s, pkt, n);
    assert(s.decoded == 1);

    /* 3) the next frame (new frame_id) decodes */
    olen = encode_tone(monoenc, 1, opus);
    n = build_pkt(pkt, s.quest_be, cloud, 40000, 0xAABBCCDD, opus, olen, 1001);
    handle_ip(&s, pkt, n);
    assert(s.decoded == 2);

    /* 4) a different SSRC on the same wire is NOT the locked mic → ignored */
    n = build_pkt(pkt, s.quest_be, cloud, 40000, 0x11112222, opus, olen, 5);
    handle_ip(&s, pkt, n);
    assert(s.decoded == 2);

    /* 5) Quest->LAN (private dst) is the remote-desktop path, never the room mic → ignored */
    struct bsdr_micsniff s2; memset(&s2, 0, sizeof s2);
    s2.dec = opus_decoder_create(48000, 1, &err); s2.quest_be = s.quest_be;
    n = build_pkt(pkt, s2.quest_be, lan, 45004, 0xAABBCCDD, opus, olen, 7);
    handle_ip(&s2, pkt, n);
    assert(s2.have_flow == 0);

    /* 6) a STEREO stream (desktop audio, not the mono mic) is not adopted as the owner mic */
    struct bsdr_micsniff s3; memset(&s3, 0, sizeof s3);
    s3.dec = opus_decoder_create(48000, 1, &err); s3.quest_be = s.quest_be;
    int slen = encode_tone(stereoenc, 2, opus); assert(slen > 0);
    assert((opus[0] & 0x04) != 0);                  /* stereo TOC bit set */
    n = build_pkt(pkt, s3.quest_be, cloud, 40001, 0x33334444, opus, slen, 1);
    handle_ip(&s3, pkt, n);
    assert(s3.have_flow == 0);

    /* 7) traffic from a different source IP never reaches the mic (src guard) */
    struct bsdr_micsniff s4; memset(&s4, 0, sizeof s4);
    s4.dec = opus_decoder_create(48000, 1, &err); s4.quest_be = s.quest_be;
    olen = encode_tone(monoenc, 1, opus);
    n = build_pkt(pkt, inet_addr("192.168.1.200"), cloud, 40000, 0x9, opus, olen, 1);
    handle_ip(&s4, pkt, n);
    assert(s4.have_flow == 0);

    opus_encoder_destroy(monoenc); opus_encoder_destroy(stereoenc);
    opus_decoder_destroy(s.dec); opus_decoder_destroy(s2.dec);
    opus_decoder_destroy(s3.dec); opus_decoder_destroy(s4.dec);
    printf("test_micsniff: all assertions passed\n");
    return 0;
}
