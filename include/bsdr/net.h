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
/* Cross-platform UDP/TCP socket helpers. */
#ifndef BSDR_NET_H
#define BSDR_NET_H

#include "bsdr/platform.h"

#include <stddef.h>

/* Close a socket portably. */
void bsdr_socket_close(bsdr_socket_t s);

/* Last socket error as a string (errno / WSAGetLastError). */
const char *bsdr_socket_strerror(void);

/* --- UDP -------------------------------------------------------------------*/
/* Create a UDP socket bound to (bind_host, port). bind_host may be NULL/"" for
 * INADDR_ANY. Sets SO_REUSEADDR; enables broadcast if `broadcast`. */
bsdr_socket_t bsdr_udp_bind(const char *bind_host, uint16_t port, bool broadcast);

int bsdr_udp_sendto(bsdr_socket_t s, const void *buf, size_t len,
                    const struct sockaddr_in *to);
/* Returns #bytes, 0 on orderly close, <0 on error. Fills `from` if non-NULL. */
int bsdr_udp_recvfrom(bsdr_socket_t s, void *buf, size_t len,
                      struct sockaddr_in *from);

/* --- TCP (for the HTTP control server) ------------------------------------*/
bsdr_socket_t bsdr_tcp_listen(const char *bind_host, uint16_t port, int backlog);
bsdr_socket_t bsdr_tcp_accept(bsdr_socket_t listener, struct sockaddr_in *from);
/* Mark a listener/discovery socket non-blocking so its accept/recvfrom poll loop
 * exits promptly on shutdown instead of blocking until the next packet. */
void bsdr_set_nonblocking(bsdr_socket_t s);
int bsdr_send_all(bsdr_socket_t s, const void *buf, size_t len);

/* Sleep until `s` is readable or `timeout_ms` elapses. Returns 1 if readable,
 * 0 on timeout, <0 on error. Lets a non-blocking-socket loop truly sleep
 * instead of busy-polling with bsdr_sleep_ms(). Portable (poll / WSAPoll). */
int bsdr_socket_wait_readable(bsdr_socket_t s, int timeout_ms);

/* --- helpers ---------------------------------------------------------------*/
bool bsdr_sockaddr_make(struct sockaddr_in *out, const char *ip, uint16_t port);
void bsdr_sockaddr_ip(const struct sockaddr_in *addr, char *out, size_t outlen);

/* Cross-platform CSPRNG (BCrypt on Windows, getrandom/urandom on POSIX). */
void bsdr_random_bytes(void *buf, size_t n);

#endif /* BSDR_NET_H */
