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
/* Cross-platform primitives: sockets, threads, sleep.
 *
 * Hides the Win32 vs POSIX split so the rest of the agent is portable across
 * Linux, Windows and macOS.
 */
#ifndef BSDR_PLATFORM_H
#define BSDR_PLATFORM_H

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
/* Windows/mingw has no <sys/uio.h>; bsdrX's batch-send API (bsdr_udp_send_batch,
 * the --sendmmsg LAN path) takes struct iovec, so provide the POSIX layout. */
struct iovec { void *iov_base; size_t iov_len; };
typedef SOCKET bsdr_socket_t;
#  define BSDR_INVALID_SOCKET INVALID_SOCKET
#  define BSDR_SOCK_ERR       SOCKET_ERROR
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int bsdr_socket_t;
#  define BSDR_INVALID_SOCKET (-1)
#  define BSDR_SOCK_ERR       (-1)
#endif

#include <stdbool.h>
#include <stdint.h>

/* One-time process init/cleanup (WSAStartup on Windows; no-op elsewhere). */
bool bsdr_platform_init(void);
void bsdr_platform_cleanup(void);

void bsdr_sleep_ms(unsigned ms);
uint64_t bsdr_now_ms(void);   /* monotonic milliseconds */

/* Minimal thread wrapper (pthreads / Win32 threads). */
typedef struct bsdr_thread bsdr_thread;
typedef void (*bsdr_thread_fn)(void *arg);

bsdr_thread *bsdr_thread_start(bsdr_thread_fn fn, void *arg);
void bsdr_thread_join(bsdr_thread *t);   /* joins and frees */
/* Fire-and-forget thread: self-frees on exit, never joined. */
bool bsdr_thread_start_detached(bsdr_thread_fn fn, void *arg);

/* Minimal mutex. */
typedef struct bsdr_mutex bsdr_mutex;
bsdr_mutex *bsdr_mutex_new(void);
void bsdr_mutex_free(bsdr_mutex *m);
void bsdr_mutex_lock(bsdr_mutex *m);
void bsdr_mutex_unlock(bsdr_mutex *m);

/* Minimal condition variable, paired with a bsdr_mutex. Lets a consumer thread
 * sleep until a producer signals instead of polling. The mutex must be held
 * across wait; wait atomically releases it while blocked and re-acquires on wake
 * (spurious wakeups possible — always re-check the predicate in a loop). */
typedef struct bsdr_cond bsdr_cond;
bsdr_cond *bsdr_cond_new(void);
void bsdr_cond_free(bsdr_cond *c);
void bsdr_cond_wait(bsdr_cond *c, bsdr_mutex *m);          /* wait indefinitely */
void bsdr_cond_wait_ms(bsdr_cond *c, bsdr_mutex *m, int timeout_ms);
void bsdr_cond_signal(bsdr_cond *c);                       /* wake one waiter */

#endif /* BSDR_PLATFORM_H */
