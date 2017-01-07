#pragma once
#include <sys/socket.h>
#include "coroutine.h"

// socket
EXPORT_API int sock_socket(int domain, int type, int protocol);

EXPORT_API int sock_accept(int, struct sockaddr *, socklen_t*);

EXPORT_API int sock_listen(int sock, int backlog);

EXPORT_API int sock_bind(int sock, const struct sockaddr *addr, socklen_t len);

EXPORT_API int sock_connect(int s_, struct sockaddr *addr, socklen_t len);

EXPORT_API ssize_t sock_recv(int sock, char *buf, size_t size);
EXPORT_API ssize_t sock_send_all(int sock, char *buf, size_t size);
EXPORT_API ssize_t sock_send(int sock, char *buf, size_t size);

EXPORT_API int sock_flush(int sock);

EXPORT_API int sock_close(int sock);

EXPORT_API int sock_setsockopt(int sock, int level, int optname, const void *optval, socklen_t optlen);

EXPORT_API int sock_getsockopt(int sock, int level, int optname, void *optval, socklen_t *optlen);

// TODO: gethostbyname系列函数，可以考虑模拟libevent实现一个evutil_getaddrinfo函数
