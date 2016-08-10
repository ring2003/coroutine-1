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

// uthread coroutine
EXPORT_API uthread_t coro_create_uthread(uthread_entry cb, void *data);

EXPORT_API int coro_start_uthread(uthread_t th);

EXPORT_API int coro_join_uthread(uthread_t th);

EXPORT_API uthread_t coro_current_uthread();

EXPORT_API int coro_current_tid_uthread();
EXPORT_API int coro_tid_uthread(uthread_t th);

// sleep
EXPORT_API int coro_sleep(int ms);


