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
#include "bsdr/net.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <process.h>
#  include <bcrypt.h>
#else
#  include <errno.h>
#  include <pthread.h>
#  include <fcntl.h>
#  if defined(__linux__) && !defined(__ANDROID__)
#    include <sys/random.h>   /* getrandom(); on Android (bionic) it's API 28+, so use /dev/urandom instead */
#  endif
#endif

/* ------------------------------------------------------------------ platform */
bool bsdr_platform_init(void) {
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void bsdr_platform_cleanup(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

void bsdr_sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

void bsdr_random_bytes(void *buf, size_t n) {
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return;
#elif defined(__linux__) && !defined(__ANDROID__)
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }  /* a signal must NOT abort the fill */
        if (r == 0) break;
        got += (size_t)r;
    }
    if (got == n) return;
    /* getrandom() failed (very early boot, seccomp, old kernel) — fall through to /dev/urandom. */
#endif
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t r = fread(buf, 1, n, f);
            fclose(f);
            if (r == n) return;
        }
    }
    /* No system CSPRNG is reachable. This output feeds pairing codes and auth tokens
     * (control.c), so a predictable rand() fallback would be a real auth-bypass — fail hard
     * instead of silently emitting guessable "secret" bytes. */
    fprintf(stderr, "bsdr.net: no secure RNG available (getrandom/urandom failed) — aborting to "
                    "avoid predictable pairing codes/tokens\n");
    abort();
}

uint64_t bsdr_now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* ------------------------------------------------------------------- threads */
struct bsdr_thread {
#if defined(_WIN32)
    HANDLE h;
#else
    pthread_t h;
#endif
    bsdr_thread_fn fn;
    void *arg;
};

#if defined(_WIN32)
static unsigned __stdcall thread_trampoline(void *p) {
    struct bsdr_thread *t = (struct bsdr_thread *)p;
    t->fn(t->arg);
    return 0;
}
#else
static void *thread_trampoline(void *p) {
    struct bsdr_thread *t = (struct bsdr_thread *)p;
    t->fn(t->arg);
    return NULL;
}
#endif

bsdr_thread *bsdr_thread_start(bsdr_thread_fn fn, void *arg) {
    struct bsdr_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
#if defined(_WIN32)
    t->h = (HANDLE)_beginthreadex(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
#else
    if (pthread_create(&t->h, NULL, thread_trampoline, t) != 0) { free(t); return NULL; }
#endif
    return t;
}

#if defined(_WIN32)
static unsigned __stdcall detached_trampoline(void *p) {
    struct bsdr_thread *t = (struct bsdr_thread *)p;
    t->fn(t->arg);
    CloseHandle(t->h);
    free(t);
    return 0;
}
#else
static void *detached_trampoline(void *p) {
    struct bsdr_thread *t = (struct bsdr_thread *)p;
    t->fn(t->arg);
    free(t);
    return NULL;
}
#endif

/* Fire-and-forget thread: self-frees on exit, never joined. */
bool bsdr_thread_start_detached(bsdr_thread_fn fn, void *arg) {
    struct bsdr_thread *t = calloc(1, sizeof(*t));
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
#if defined(_WIN32)
    t->h = (HANDLE)_beginthreadex(NULL, 0, detached_trampoline, t, 0, NULL);
    if (!t->h) { free(t); return false; }
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->h, &attr, detached_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return false; }
#endif
    return true;
}

void bsdr_thread_join(bsdr_thread *t) {
    if (!t) return;
#if defined(_WIN32)
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
#else
    pthread_join(t->h, NULL);
#endif
    free(t);
}

/* -------------------------------------------------------------------- mutex */
struct bsdr_mutex {
#if defined(_WIN32)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
};

bsdr_mutex *bsdr_mutex_new(void) {
    struct bsdr_mutex *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
#if defined(_WIN32)
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->m, NULL);
#endif
    return m;
}
void bsdr_mutex_free(bsdr_mutex *m) {
    if (!m) return;
#if defined(_WIN32)
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->m);
#endif
    free(m);
}
void bsdr_mutex_lock(bsdr_mutex *m) {
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->m);
#endif
}
void bsdr_mutex_unlock(bsdr_mutex *m) {
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->m);
#endif
}

/* ------------------------------------------------------------------ sockets */
void bsdr_socket_close(bsdr_socket_t s) {
    if (s == BSDR_INVALID_SOCKET) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

const char *bsdr_socket_strerror(void) {
#if defined(_WIN32)
    static char buf[128];
    int err = WSAGetLastError();
    snprintf(buf, sizeof(buf), "winsock error %d", err);
    return buf;
#else
    return strerror(errno);
#endif
}

bool bsdr_sockaddr_make(struct sockaddr_in *out, const char *ip, uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (!ip || !*ip) {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    return inet_pton(AF_INET, ip, &out->sin_addr) == 1;
}

void bsdr_sockaddr_ip(const struct sockaddr_in *addr, char *out, size_t outlen) {
    inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)outlen);
}

static void set_reuseaddr(bsdr_socket_t s) {
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
}

/* Mark a listener/discovery socket non-blocking so its accept/recvfrom poll loop
 * can observe a shutdown flag instead of blocking forever with no traffic (a
 * blocked accept/recvfrom is NOT woken by close() from another thread). */
void bsdr_set_nonblocking(bsdr_socket_t s) {
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl != -1) fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

bsdr_socket_t bsdr_udp_bind(const char *bind_host, uint16_t port, bool broadcast) {
    bsdr_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BSDR_INVALID_SOCKET) return BSDR_INVALID_SOCKET;
    set_reuseaddr(s);
    if (broadcast) {
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    }
    struct sockaddr_in addr;
    bsdr_sockaddr_make(&addr, bind_host, port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == BSDR_SOCK_ERR) {
        bsdr_socket_close(s);
        return BSDR_INVALID_SOCKET;
    }
    return s;
}

int bsdr_udp_sendto(bsdr_socket_t s, const void *buf, size_t len,
                    const struct sockaddr_in *to) {
    return (int)sendto(s, (const char *)buf, (int)len, 0,
                       (const struct sockaddr *)to, sizeof(*to));
}

int bsdr_udp_recvfrom(bsdr_socket_t s, void *buf, size_t len,
                      struct sockaddr_in *from) {
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = (int)recvfrom(s, (char *)buf, (int)len, 0,
                          (struct sockaddr *)&src, &slen);
    if (n >= 0 && from) *from = src;
    return n;
}

bsdr_socket_t bsdr_tcp_listen(const char *bind_host, uint16_t port, int backlog) {
    bsdr_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BSDR_INVALID_SOCKET) return BSDR_INVALID_SOCKET;
    set_reuseaddr(s);
    struct sockaddr_in addr;
    bsdr_sockaddr_make(&addr, bind_host, port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == BSDR_SOCK_ERR ||
        listen(s, backlog) == BSDR_SOCK_ERR) {
        bsdr_socket_close(s);
        return BSDR_INVALID_SOCKET;
    }
    return s;
}

bsdr_socket_t bsdr_tcp_accept(bsdr_socket_t listener, struct sockaddr_in *from) {
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    bsdr_socket_t c = accept(listener, (struct sockaddr *)&src, &slen);
    if (c != BSDR_INVALID_SOCKET && from) *from = src;
    return c;
}

int bsdr_send_all(bsdr_socket_t s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (int)sent;
}
