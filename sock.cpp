#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <netinet/tcp.h>

#include <atomic>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <evutil.h>

#include "internal.h"
#include "coroutine.h"
#include "util.h"

// 约定：
//   1. 非读写的EVENT事件导致的socket状态变更
//   由event回调来修改(CONNECT ACCEPT EOF ERROR)
//   2. 读写EVENT事件导致的socket状态变更
//   统一在sock_$op函数上下文设置
//   （上半段设置状态，下半段更新状态）


static coro_sock *find_sock_by_fd(int fd)
{
    coro_sock *sock = ctx.socks[fd];
    assert(sock);
    return sock;
}

static void set_io_status(unsigned int &status, io_status s)
{
    switch ( s ) {
    case set_wait_read_status:
        SET_WAIT_READ(status);
        break;
    case set_wait_write_status:
        SET_WAIT_WRITE(status);
        break;
    case set_read_status:
        SET_READ(status);
        break;
    case set_write_status:
        SET_WRITE(status);
        break;
    case set_wait_accept_status:
        SET_WAIT_ACCEPT(status);
        break;
    case set_wait_io_status:
        SET_WAIT_IO(status);
        break;
    case set_wait_connect_status:
        SET_WAIT_CONNECT(status);
        break;
    case clear_status:
        CLEAR_MASK(status);
        break;
    default:
        break;
    }
}

static void set_pending_status(coro_sock *sock, io_status sock_status, uthread_t tid, io_status th_status)
{
    set_io_status(sock->status, sock_status);
    uthread *th = ctx.ths[tid];
    th->pending_sock = sock;
    set_io_status(th->status, th_status);
}

static void clear_pending_status(coro_sock *sock, uthread_t tid)
{
    (void)sock;
    CLEAR_MASK(ctx.ths[tid]->status);
}

// read event for all socket
static void sock_buffer_read_event(bufferevent *bev, void *arg)
{
    coro_event ev;
    coro_sock *sock = (coro_sock *)arg;
    ev.sock = bufferevent_getfd(bev);
    ev.event = SOCK_READ_NOTIFY;
    sock->ctx->curev = ev;
    coro_yield_uthread(ctx.io, 0);
}

// write event for all socket
static void sock_buffer_write_event(bufferevent *bev, void *arg)
{
    coro_event ev;
    coro_sock *sock = (coro_sock *)arg;
    ev.sock = bufferevent_getfd(bev);
    ev.event = SOCK_WRITE_NOTIFY;
    sock->ctx->curev = ev;
    coro_yield_uthread(ctx.io, 0);
}

// error event socket ( all client socket)
// connecting socket (client side client)
static void sock_buffer_error_event(bufferevent *bev, short what, void *arg)
{
    coro_sock *sock = (coro_sock *)arg;
    coro_event ev;
    ev.sock = bufferevent_getfd(bev);
    ev.sock = sock->sock;
    if ( what & BEV_EVENT_ERROR ) {
        SET_ERROR(sock->status);
        ev.event = SOCK_ERROR_NOTIFY;
    }
    else if ( what & BEV_EVENT_EOF ) {
        ev.event = SOCK_EOF_NOTIFY;
        SET_EOF(sock->status);
        if ( evbuffer_get_length(bufferevent_get_input(sock->bev)) ) {
            SET_READ(sock->status);
            printf("set read: %d\n", ev.sock);
        }
    }
    else {
        ; // pass
    }
    sock->ctx->curev = ev;
    coro_yield_uthread(ctx.io, 0);
}

// for accept event
static void sock_raw_accept_event(int s, short what, void *arg)
{
    (void)what;
    (void)s;
    global_context *ctx = (global_context *)arg;
    coro_event ev;
    ev.event = SOCK_ACCEPT_NOTIFY;
    ev.sock = s;
    ctx->curev = ev;
    coro_yield_uthread(ctx->io, 0);
}

///////////////////////////////////////////////
// coro socket interface
///////////////////////////////////////////////
int sock_flush(int fd)
{
    coro_sock *sock = find_sock_by_fd(fd);
    if ( !sock ) {
        return -1;
    }
    bufferevent_disable(sock->bev, EV_READ);
    auto *buf = bufferevent_get_output(sock->bev);
    uthread_t cur = coro_current_uthread();
    size_t left = 0;
    int ret = 0;
    do {
        coro_sock *sock = find_sock_by_fd(fd);
        if ( !sock ) {
            return -1;
        }
        left = evbuffer_get_length(buf);
        if ( left > 0 ) {
            set_pending_status(sock, set_wait_write_status, cur, set_wait_write_status);
            sock->writequeue->push(cur);
            int status = coro_schedule_uthread(cur, 0);
            if ( status < 0 ) {
                ret = status;
                break;
            }
        }
    } while(left);
    return ret;
}

// 参考sock_recv
ssize_t sock_send(int fd, char *buf, size_t size)
{
    coro_sock *sock = find_sock_by_fd(fd);
    if ( !sock ) {
        return -1;
    }
    sock->op = WRITING;
    ssize_t status = 0;
    uthread_t cur = coro_current_uthread();
    if ( TEST_WAIT_WRITE(sock->status) ) {
        if ( TEST_EOF(sock->status) ) {
            status = -1;
        }
        else {
            set_pending_status(sock, keep_status, cur, set_wait_write_status);
            sock->writequeue->push(cur);
            status = coro_schedule_uthread(cur, 0);
        }
    }
    int ret = status;
    sock = find_sock_by_fd(fd);
    if ( status >= 0 && sock ) {
        // 1. 协程没有进行调度，那么肯定可写
        // 2. 协程唤醒以后
        //   1) 写事件成功，那么一定可写
        //   2) 写事件失败，那么status < 0，不可写
        //      这种情况下，一定执行过调度的环节
        //      调度环节执行前已经设置了等待写状态
        //      因此，status < 0的情况不需要设置了
        SET_WRITE(sock->status);
        clear_pending_status(sock, cur);

        size_t cnt = 0;
        struct evbuffer *output;
        bufferevent * bev = sock->bev;
        output = bufferevent_get_output(bev);
        if ( sock->hwm ) {
            size_t buflen = evbuffer_get_length(output);
            cnt = sock->hwm - buflen;
            size = size > cnt ? cnt : size;
        }
        ret = evbuffer_add(output, buf, size);
        ret = !ret ? size : -1;
    }
    return ret;
}

// 参考sock_recv
// 我们认为，所有的pending队列都在sock_send里面设置
// sock_send_all不设置任何的sock的pengding队列
// XXX:
// 如果第一次sock_send_all分多次send
// 任何一次send失败，都当做失败
ssize_t sock_send_all(int fd, char *buf, size_t size)
{
    coro_sock *sock = find_sock_by_fd(fd);
    if ( !sock ) {
        return -1;
    }
    ssize_t cnt = 0;
    ssize_t status = 0;
    uthread_t cur = coro_current_uthread();
    while ( (size_t)cnt < size ) {
        if ( status < 0 ) {
            cnt = -1;
            break;
        }
        ssize_t send_cnt = sock_send(sock->sock, buf + cnt, size);
        // 如果某次send_all过程中出错，认定为全部send失败
        if ( send_cnt < 0 ) {
            return -1;
        }
        cnt += send_cnt;
        if ( (size_t)cnt < size ) {
            set_pending_status(sock, set_wait_write_status, cur, set_wait_write_status);
            status = coro_schedule_uthread(cur, 0);
        }
    }
    clear_pending_status(sock, cur);
    return cnt;
}

ssize_t sock_recv(int fd, char *buf, size_t size)
{
    coro_sock *sock = find_sock_by_fd(fd);
    // 一个socket可能被多个协程持有，而这多个持有当前socket的协程，可能
    // 当前并没有阻塞再这个socket上，考虑如下情况，某个持有当前socket的协程
    // 释放了这个socket对象，那么，当某个持有这个socket的协程在使用这个socket的
    // 时候，就可能无法通过fd反查socket了
    // 所以，这里要重新尝试获取socket，判断socket对象是否有效
    // 这里可能会有bug产生，因为fd是每次取最小的，如果close掉了，立马又创建了一个socket
    // 那么2个fd是重复，因此，完善的解决方案大概是这样子的
    // 但是这个情况在非协程环境也是存在的，因此，不做针对他的特殊处理了
    if ( !sock ) {
        return -1;
    }
    sock->op = READING;
    ssize_t status = 0;
    // 调用sock_recv的时候，还没有进行yield
    // 所以，当前协程就是持有当前socket对象的协程
    // 因此，我们可以把当前协程加到socket的持有协程里面去
    uthread_t cur = coro_current_uthread();
    if ( TEST_WAIT_READ(sock->status) ) {
        if ( TEST_EOF(sock->status) ) {
            status = -1;
            // SET_EOF(sock->status);
        }
        else {
            // 如果当前socket在等待读
            // 那么说明当前协程已经无法继续往下执行了，
            //  1. 协程会阻塞在当前socket的读队列里面
            //  2. socket被阻塞在当前协程
            //  发生了阻塞，那么我们需要切换到其他的协程上去，放弃继续执行机会
            set_pending_status(sock, set_wait_read_status, cur, set_wait_read_status);
            sock->readqueue->push(cur);
            status = coro_schedule_uthread(cur, 0);
        }
    }
    // 执行到这里，说明我们完成了IO操作
    // 那么说明，我们的协程当前一定没有被阻塞，所以移除阻塞标志

    // 假设read被阻塞了，那么，执行到这里一定是经历过调度，然后回来的
    // 那么，我们就无法保证这个sock指针还是有效的，因为他可能被释放过了
    // 因此这里要重新获取sock指针

    ssize_t ret = status;
    sock = find_sock_by_fd(fd);
    if ( ret >= 0 && sock ) {

        bufferevent * bev = sock->bev;
        struct evbuffer *input;
        input = bufferevent_get_input(bev);
        int n = evbuffer_remove(input, buf, size);
        // 无论如何，我们都认为这次read已经结束了
        clear_pending_status(sock, cur);
        // 如果inputbuffer还有剩余空间，那么我们留下可读标记
        if ( n >= 0 && evbuffer_get_length(input) > 0 ) {
            SET_READ(sock->status);
        }
        ret = (n >= 0 ? n : -1);
    }
    return ret;
}

int sock_socket(int domain, int type, int protocol)
{
    int s = socket(domain, type, protocol);
    coro_sock *sock = NULL;
    if ( s >= 0 ) {
        evutil_make_socket_nonblocking(s);
        sock = new coro_sock;
        sock->ctx = &ctx;
        sock->bev = NULL;
        sock->lwm = 0;
        sock->hwm = 0;
        sock->status = 0;
        sock->sock = s;
        ctx.socks[s] = sock;
        sock->readqueue = std::make_shared<std::queue<uthread_t>>();
        sock->writequeue = std::make_shared<std::queue<uthread_t>>();
        sock->eventqueue = std::make_shared<std::queue<uthread_t>>();
    }
    return s;
}

static coro_sock * sock_assign(int fd, bufferevent *bev)
{
    coro_sock *sock = new coro_sock;
    sock->ctx = &ctx;
    sock->bev = bev;
    sock->lwm = 0;
    sock->hwm = 0;
    sock->status = 0;
    sock->sock = fd;
    if ( fd >= 0 ) {
        ctx.socks[fd] = sock;
    }
    return sock;
}

int sock_close(int fd)
{
    int ret = 0;
    coro_sock *sock = ctx.socks[fd];
    if ( sock ) {
        if ( sock->bev ) {
            bufferevent_free(sock->bev);
        }
        else {
            if ( sock->sock >= 0 ) {
                ret = close(sock->sock);
            }
        }
        ctx.socks[fd] = NULL;
        delete sock;
    }
    return ret;
}

int sock_connect(int s, struct sockaddr *addr, socklen_t len)
{
    event_base *base = ctx.base;
    struct bufferevent *bev = bufferevent_socket_new(base, s, BEV_OPT_CLOSE_ON_FREE);
    coro_sock *sock = ctx.socks[s];
    sock->op = CONNECTING;
    SET_WAIT_CONNECT(sock->status);
    bufferevent_setcb(bev, sock_buffer_read_event, sock_buffer_write_event, sock_buffer_error_event, sock);
    sock->bev = bev;
    int ret = connect(s, addr, len);
    if ( ret < 0 ) {
        if ( errno == EINTR || errno == EINPROGRESS ) {
            bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);
            uthread_t cur = coro_current_uthread();
            REMOVE_WAIT_CONNECT(sock->status);
            set_pending_status(sock, set_wait_write_status, cur, set_wait_write_status);
            sock->writequeue->push(cur);
            ret = coro_schedule_uthread(cur, 0);
        }
        else if ( errno == ECONNREFUSED ) {
            // nothing to do
            // sock_close will free bufferevent
            ret = -1;
        }
    }
    else {
        REMOVE_WAIT_CONNECT(sock->status);
        SET_WAIT_WRITE(sock->status);
        uthread_t cur = coro_current_uthread();

        bufferevent_enable(bev, EV_READ | EV_WRITE);
        clear_pending_status(sock, cur);
    }
    return ret;
}

static int do_accept(int sock, struct sockaddr *addr, socklen_t *len)
{
    int c = -1;
    while ( true ) {
        c = accept4(sock, addr, len, SOCK_NONBLOCK);
        if ( c >= 0 ) {
            break;
        }
        if ( errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) {
            continue;
        }
        break;
    }
    return c;
}

static coro_sock* sock_accept_inner(coro_sock *sock, struct sockaddr *addr, socklen_t *len)
{
    event_base *base = ctx.base;
    uthread_t cur = coro_current_uthread();
    struct event * ev = (struct event *)coro_uthread_get_data(cur);
    if ( ev == NULL ) {
        ev = event_new(base, sock->sock, EV_READ | EV_PERSIST, sock_raw_accept_event, (void *)&ctx);
        coro_uthread_set_data(cur, ev, (free_data)event_free);
    }
    event_add(ev, NULL);
    // 如果accept操作是主线程操作的，我们应该resume IO协程
    // 让IO协程帮我们处理事件，完成accept通知
    // 如果accept操作是协程处理的，那么切回协程
    // 参看sock_accept_event_cb的切换规则，
    // 因为WAIT_ACCEPT状态只有sock_raw_accept_event才能解除
    // 因此这里这里一定是sock_raw_accept_event切换回来
    // 因此，resume和yield返回值一定是sock_raw_accept_event返回的socket fd
    int ret = coro_schedule_uthread(cur, 0);
    event_del(ev);
    int s = sock->sock;
    coro_sock *client = NULL;
    if ( ret >= 0 ) {
        int c = do_accept(s, addr, len);
        if ( c >= 0 ) {
            bufferevent *bev = bufferevent_socket_new(base, c, BEV_OPT_CLOSE_ON_FREE);
            client = sock_assign(c, bev);

            bufferevent_setcb(bev, sock_buffer_read_event, sock_buffer_write_event, sock_buffer_error_event, (void *)client);
            bufferevent_enable(bev, EV_READ | EV_WRITE);
        }
    }
    return client;
}

int sock_accept(int fd, struct sockaddr *, socklen_t*)
{
    coro_sock *sock = find_sock_by_fd(fd);
    sock->op = ACCEPTING;
    coro_sock *client = NULL;
    uthread_t cur = coro_current_uthread();
    set_pending_status(sock, set_wait_accept_status, cur, set_wait_accept_status);
    sock->eventqueue->push(cur);
    client = sock_accept_inner(sock, NULL, NULL);
    // 对于一个accept成功的socket，我们认为他是可以写的
    // 因此我们给他增加写入标志
    clear_pending_status(client, cur);
    CLEAR_MASK(client->status);
    SET_WRITE(client->status);
    return client->sock;
}

int sock_listen(int fd, int backlog)
{
    return listen(fd, backlog);
}

int sock_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return bind(fd, addr, len);
}

int sock_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    return setsockopt(fd, level, optname, optval, optlen);
}

int sock_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    return getsockopt(fd, level, optname, optval, optlen);
}
