#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <string>
#include <vector>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <netinet/tcp.h>

#include "sock.h"
#include "lock.h"

coro_lock_t lock;

///////////////////////////////////////////////
// utils
///////////////////////////////////////////////
static int get_ip_port(const char *addr, char *ip, size_t len, int *port)
{
    const char * pos = strchr(addr, ':');
    memset(ip, 0, len);
    memcpy(ip, addr, pos - addr);
    *port = atoi(++pos);
    return 0;
}

struct sockaddr_in get_addr(const char *addr)
{
    char ip[32];
    int port;
    get_ip_port(addr, ip, sizeof(ip), &port);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_addr.s_addr = inet_addr(ip);
    return sin;
}

int entry(void *arg)
{
    const char *addr = (const char *)arg;
    int sock = sock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    int val = 1;
    sock_setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in sin = get_addr(addr);
    sock_bind(sock, (struct sockaddr*)&sin, sizeof(sin));
    sock_listen(sock, 1024);

    int c;
    printf("wait accept begin...\n");
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    c = sock_accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
    printf("wait accept done\n");
    char buf[128];
    memset(buf, 0, 128);
    printf("recv begin...\n");
    int n = sock_recv(c, buf, 128);
    printf("recv done\n");
    printf("send begin...\n");
    n = sock_send(c, buf, n);
    printf("send done\n");
    sock_flush(c);

    sock_close(c);
    sock_close(sock);
    printf("done\n");
    return 0;
}

int entry2(void *arg)
{
    (void)arg;
    struct sockaddr_in sin = get_addr("127.0.0.1:8080");
    int client = sock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ( client < 0 ) {
        return -1;
    }
    int ret = sock_connect(client, (struct sockaddr*)&sin, sizeof(sin));
    if ( ret < 0 ) {
        sock_close(client);
        return -1;
    }
    char buf[1024];
    strcpy(buf, "GET / HTTP/1.1\r\n\r\n");
    int len = strlen(buf);
    int l = sock_send_all(client, buf, len);
    if ( l < 0 ) {
        sock_close(client);
        return -1;
    }
    // printf("send: %d\n", l);
    l = sock_recv(client, buf, 1024);
    if ( l < 0 ) {
        sock_close(client);
        return -1;
    }
    buf[l] = '\0';
    // printf("recvbuf: %s\n", buf);
    sock_close(client);
    return 0;
}

int entry3(void *arg)
{
    (void)arg;
    coro_uthread_mutex_lock(&lock);
    uthread_t th = coro_create_uthread(entry2, NULL);
    coro_start_uthread(th);
    coro_join_uthread(th);
    coro_uthread_mutex_unlock(&lock);
    printf("wait sub entry3 end\n");
    return 0;
}

int entry5(void *arg)
{
    (void)arg;
    coro_uthread_mutex_lock(&lock);
    uthread_t th = coro_create_uthread(entry, arg);
    coro_start_uthread(th);
    coro_join_uthread(th);
    coro_uthread_mutex_unlock(&lock);
    printf("wait sub entry5 end\n");
    return 0;
}

int entry4(void *arg)
{
    coro_uthread_mutex_lock(&lock);
    const char *addr = (const char *)arg;
    int sock = sock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    int val = 1;
    sock_setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in sin = get_addr(addr);
    sock_bind(sock, (struct sockaddr*)&sin, sizeof(sin));
    sock_listen(sock, 1024);
    while ( true ) {
        coro_sleep(1000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int n = 500;
    if ( argc > 1 ) {
        n = std::stoi(argv[1]);
    }

    std::vector<uthread_t> tids;
    printf("start main\n");
    const char addr1[] = "127.0.0.1:30086";
    const char addr2[] = "127.0.0.1:20086";
    const char addr3[] = "127.0.0.1:40086";
    for ( int j = 0; j < 1000; j++ ) {
        for ( int i = 0; i < n; i++ ) {
            uthread_t tid = coro_create_uthread(entry2, NULL);
            coro_start_uthread(tid);
            tids.push_back(tid);
        }
        for ( int i = 0; i < n; i++ ) {
            coro_join_uthread(tids[j * n + i]);
        }
    }
    printf("done\n");
#if 0
    uthread_t th1 = coro_create_uthread(entry, (void *)&addr1);
    uthread_t th2 = coro_create_uthread(entry2, NULL);
    uthread_t th3 = coro_create_uthread(entry3, (void *)&addr2);
    uthread_t th5 = coro_create_uthread(entry5, (void *)&addr3);
    uthread_t th4 = coro_create_uthread(entry2, NULL);
    coro_uthread_mutex_init(&lock);
    coro_start_uthread(th1);
    coro_join_uthread(th1);
    printf("joined 1\n");
    coro_start_uthread(th2);
    coro_join_uthread(th2);
    printf("joined 2\n");
    coro_start_uthread(th3);
    coro_join_uthread(th3);
    printf("joined 3\n");
    coro_start_uthread(th4);
    printf("join 4...\n");
    coro_join_uthread(th4);
    printf("joined 4\n");
    coro_start_uthread(th5);
    printf("main wait join th5\n");
    coro_join_uthread(th5);
    coro_uthread_mutex_release(&lock);
#endif
    return 0;
}
