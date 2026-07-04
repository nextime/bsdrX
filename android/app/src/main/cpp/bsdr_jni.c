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
/* JNI bridge: Kotlin NativeBridge <-> the portable agent core.
 *
 *   Kotlin -> native: nativeStart/Stop, nativePushVideo (MediaCodec AUs),
 *     nativePushAudio (AudioPlaybackCapture PCM), nativePollVideoWant.
 *   native -> Kotlin: onPairingCode, onMicPcm (Quest mic -> AudioTrack),
 *     onInputEvent (-> AccessibilityService), via cached method IDs.
 *
 * The agent runs on its own native thread (bsdr_agent_run blocks); a TLS key
 * detaches any agent/worker thread from the JVM when it exits. */
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "bsdr/agentlib.h"
#include "bsdr/log.h"
#include "bsdr_android.h"

static JavaVM       *g_vm;
static jobject       g_bridge;           /* global ref to the NativeBridge singleton */
static jmethodID     g_mid_pairing;      /* onPairingCode(String) */
static jmethodID     g_mid_mic;          /* onMicPcm(short[], int, int) */
static jmethodID     g_mid_input;        /* onInputEvent(int, int, int) */
static jmethodID     g_mid_vstate;       /* onVoiceState(int) */
static jmethodID     g_mid_vfeedback;    /* onVoiceFeedback(String) */
static jmethodID     g_mid_ccactive;     /* onCompctlActive(boolean) */
static jmethodID     g_mid_shot;         /* grabScreenshot(int) -> byte[] */

static pthread_key_t g_tls;              /* per-thread JNIEnv; destructor detaches */
static pthread_t     g_agent_thr;
static int           g_agent_running;
static bsdr_agent_options g_opt;

static void tls_detach(void *env) {
    if (env && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    pthread_key_create(&g_tls, tls_detach);
    return JNI_VERSION_1_6;
}

/* Get a JNIEnv for the current thread, attaching (once) if needed. The TLS
 * destructor detaches on thread exit, so callers never detach explicitly. */
static JNIEnv *get_env(void) {
    if (!g_vm) return NULL;
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != 0) return NULL;
    pthread_setspecific(g_tls, env);
    return env;
}

/* ---- native -> Kotlin callbacks ------------------------------------------*/
static void on_pairing(const char *code, void *user) {
    (void)user;
    JNIEnv *e = get_env();
    if (!e || !g_bridge) return;
    jstring s = (*e)->NewStringUTF(e, code);
    (*e)->CallVoidMethod(e, g_bridge, g_mid_pairing, s);
    (*e)->DeleteLocalRef(e, s);
}

void bsdr_android_emit_mic(const int16_t *pcm, int frames, int channels) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge) return;
    jsize n = (jsize)frames * channels;
    jshortArray arr = (*e)->NewShortArray(e, n);
    if (!arr) return;
    (*e)->SetShortArrayRegion(e, arr, 0, n, pcm);
    (*e)->CallVoidMethod(e, g_bridge, g_mid_mic, arr, frames, channels);
    (*e)->DeleteLocalRef(e, arr);
}

void bsdr_android_emit_input(int kind, int a, int b) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge) return;
    (*e)->CallVoidMethod(e, g_bridge, g_mid_input, kind, a, b);
}

/* ---- voice computer control: native -> Kotlin ----------------------------*/
void bsdr_android_emit_voice_state(int state) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge) return;
    (*e)->CallVoidMethod(e, g_bridge, g_mid_vstate, state);
}

void bsdr_android_emit_voice_feedback(const char *msg) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge || !msg) return;
    jstring s = (*e)->NewStringUTF(e, msg);
    (*e)->CallVoidMethod(e, g_bridge, g_mid_vfeedback, s);
    (*e)->DeleteLocalRef(e, s);
}

void bsdr_android_emit_compctl_active(int active) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge) return;
    (*e)->CallVoidMethod(e, g_bridge, g_mid_ccactive, active ? JNI_TRUE : JNI_FALSE);
}

/* ---- vision screenshot: native (voice worker) -> Kotlin, blocking ---------*/
int bsdr_android_screenshot(int max_dim, uint8_t *out, size_t cap) {
    JNIEnv *e = get_env();
    if (!e || !g_bridge || !g_mid_shot) return 0;
    jbyteArray arr = (jbyteArray)(*e)->CallObjectMethod(e, g_bridge, g_mid_shot, (jint)max_dim);
    if (!arr) return 0;
    jsize n = (*e)->GetArrayLength(e, arr);
    int ret = 0;
    if (n > 0 && (size_t)n <= cap) {
        (*e)->GetByteArrayRegion(e, arr, 0, n, (jbyte *)out);
        ret = (int)n;
    } else if (n > 0) {
        BSDR_WARN("bsdr.jni", "screenshot jpeg %d > cap %zu", (int)n, cap);
    }
    (*e)->DeleteLocalRef(e, arr);
    return ret;
}

/* ---- the agent thread -----------------------------------------------------*/
static void *agent_thread(void *arg) {
    (void)arg;
    get_env();                      /* attach for the agent's lifetime (TLS detaches) */
    bsdr_agent_run(&g_opt);
    return NULL;
}

/* ---- Kotlin -> native (NativeBridge) -------------------------------------*/
#define NB(name) Java_net_nexlab_bsdrandroid_NativeBridge_##name

JNIEXPORT void JNICALL NB(nativeStart)(JNIEnv *env, jobject thiz,
                                       jint w, jint h, jint fps, jint bitrate, jint uiPort) {
    if (g_agent_running) return;

    if (!g_bridge) {
        g_bridge = (*env)->NewGlobalRef(env, thiz);
        jclass cls = (*env)->GetObjectClass(env, thiz);
        g_mid_pairing   = (*env)->GetMethodID(env, cls, "onPairingCode", "(Ljava/lang/String;)V");
        g_mid_mic       = (*env)->GetMethodID(env, cls, "onMicPcm", "([SII)V");
        g_mid_input     = (*env)->GetMethodID(env, cls, "onInputEvent", "(III)V");
        g_mid_vstate    = (*env)->GetMethodID(env, cls, "onVoiceState", "(I)V");
        g_mid_vfeedback = (*env)->GetMethodID(env, cls, "onVoiceFeedback", "(Ljava/lang/String;)V");
        g_mid_ccactive  = (*env)->GetMethodID(env, cls, "onCompctlActive", "(Z)V");
        g_mid_shot      = (*env)->GetMethodID(env, cls, "grabScreenshot", "(I)[B");
    }

    bsdr_android_capture_init();
    bsdr_android_audio_init();

    bsdr_agent_options_default(&g_opt);
    g_opt.lan_live        = true;       /* Android casts via the BigSoup LAN wire format */
    g_opt.video           = true;
    g_opt.audio           = true;
    g_opt.initiator       = false;      /* device is the DTLS server (like the PC host) */
    g_opt.open_browser    = false;      /* the in-app WebView is the control surface */
    g_opt.webui_port      = uiPort;
    g_opt.screen_w        = w;
    g_opt.screen_h        = h;
    g_opt.fps             = fps;
    g_opt.bitrate         = bitrate;
    g_opt.on_pairing_code = on_pairing;

    if (pthread_create(&g_agent_thr, NULL, agent_thread, NULL) == 0)
        g_agent_running = 1;
    else
        BSDR_ERROR("bsdr.jni", "failed to spawn the agent thread");
}

JNIEXPORT void JNICALL NB(nativeStop)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (!g_agent_running) return;
    bsdr_agent_stop();
    pthread_join(g_agent_thr, NULL);
    g_agent_running = 0;
}

JNIEXPORT void JNICALL NB(nativePushVideo)(JNIEnv *env, jobject thiz,
                                           jbyteArray buf, jint len, jlong ptsUs, jboolean isConfig) {
    (void)thiz;
    if (len <= 0) return;
    uint8_t *tmp = malloc((size_t)len);
    if (!tmp) return;
    (*env)->GetByteArrayRegion(env, buf, 0, len, (jbyte *)tmp);
    bsdr_android_push_video(tmp, (size_t)len, (int64_t)ptsUs, isConfig ? 1 : 0);
    free(tmp);
}

JNIEXPORT void JNICALL NB(nativePushAudio)(JNIEnv *env, jobject thiz,
                                           jshortArray pcm, jint frames, jint channels) {
    (void)thiz;
    jsize n = (jsize)frames * channels;
    if (n <= 0) return;
    int16_t *tmp = malloc((size_t)n * sizeof(int16_t));
    if (!tmp) return;
    (*env)->GetShortArrayRegion(env, pcm, 0, n, tmp);
    bsdr_android_push_audio(tmp, frames, channels);
    free(tmp);
}

JNIEXPORT jboolean JNICALL NB(nativePollVideoWant)(JNIEnv *env, jobject thiz, jintArray out) {
    (void)thiz;
    int w = 0, h = 0, fps = 0, br = 0;
    if (!bsdr_android_capture_want(&w, &h, &fps, &br)) return JNI_FALSE;
    jint v[4] = { w, h, fps, br };
    (*env)->SetIntArrayRegion(env, out, 0, 4, v);
    return JNI_TRUE;
}

/* 2D->3D config poll: out = [mode, deepness, convergence, swap, full]. TRUE if changed. */
JNIEXPORT jboolean JNICALL NB(nativePollThreed)(JNIEnv *env, jobject thiz, jintArray out) {
    (void)thiz;
    int mode = 0, deep = 0, conv = 0, swap = 0, full = 0;
    if (!bsdr_android_poll_threed(&mode, &deep, &conv, &swap, &full)) return JNI_FALSE;
    jint v[5] = { mode, deep, conv, swap, full };
    (*env)->SetIntArrayRegion(env, out, 0, 5, v);
    return JNI_TRUE;
}

/* ---- voice computer control: Kotlin -> native ----------------------------*/
JNIEXPORT void JNICALL NB(nativePushVoiceMic)(JNIEnv *env, jobject thiz,
                                              jshortArray pcm, jint frames) {
    (void)thiz;
    if (frames <= 0) return;
    int16_t *tmp = malloc((size_t)frames * sizeof(int16_t));   /* mono */
    if (!tmp) return;
    (*env)->GetShortArrayRegion(env, pcm, 0, frames, tmp);
    bsdr_android_push_voice(tmp, frames);
    free(tmp);
}

JNIEXPORT void JNICALL NB(nativeVoiceTrigger)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz; bsdr_android_voice_trigger();
}
JNIEXPORT void JNICALL NB(nativeVoiceStop)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz; bsdr_android_voice_stop();
}
JNIEXPORT void JNICALL NB(nativeVoiceConfirm)(JNIEnv *env, jobject thiz, jboolean send) {
    (void)env; (void)thiz; bsdr_android_voice_confirm(send ? 1 : 0);
}
