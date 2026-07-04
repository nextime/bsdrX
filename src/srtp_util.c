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
#include "bsdr/srtp_util.h"
#include "bsdr/log.h"

#include <string.h>

static int g_srtp_users = 0;

bool bsdr_srtp_global_init(void) {
    if (g_srtp_users == 0) {
        if (srtp_init() != srtp_err_status_ok) {
            BSDR_ERROR("bsdr.srtp", "srtp_init failed");
            return false;
        }
    }
    g_srtp_users++;
    return true;
}

void bsdr_srtp_global_shutdown(void) {
    if (g_srtp_users > 0 && --g_srtp_users == 0) srtp_shutdown();
}

bool bsdr_srtp_session_create(srtp_t *out, const uint8_t *master,
                              bsdr_srtp_profile profile, uint32_t ssrc,
                              bool inbound) {
    srtp_policy_t pol;
    memset(&pol, 0, sizeof(pol));
    if (profile == BSDR_SRTP_AES128_CM_SHA1_80) {
        srtp_crypto_policy_set_rtp_default(&pol.rtp);
        srtp_crypto_policy_set_rtcp_default(&pol.rtcp);
    } else if (profile == BSDR_SRTP_AEAD_AES_128_GCM) {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtcp);
    } else {
        return false;
    }
    if (inbound) {
        pol.ssrc.type = ssrc_any_inbound;   /* accept the peer's arbitrary SSRC */
    } else {
        pol.ssrc.type = ssrc_specific;
        pol.ssrc.value = ssrc;
    }
    pol.key = (unsigned char *)master;
    pol.next = NULL;
    return srtp_create(out, &pol) == srtp_err_status_ok;
}
