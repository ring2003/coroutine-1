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

// XXX:
// 约定：
//   1. 非读写的EVENT事件导致的socket状态变更
//   由event回调来修改(CONNECT ACCEPT EOF ERROR)
//   2. 读写EVENT事件导致的socket状态变更
//   统一在sock_$op函数上下文设置
//   （上半段设置状态，下半段更新状态）


// 增加一个调度协程，IO协程收到事件以后，第一次时间切换到调度协程
// IO协程改成由调度协程启动
// 然后调度协程负责把各种event里面的事件解析，然后去调度其他协程
// 当协程自然退出，其他的协程都和主协程都在等待join的时候
// 这个时候回回到调度协程或者主协程，而不是IO协程
// 因为所有的工作协程都是被调度协程或者主协程唤醒的
// 调度协程是肯定不会被IO和join，sleep事件阻塞的
// 因此，调度协程在发现有协程退出以后
// 可以检查是否有协程阻塞在join事件，如果有调度
// 如果没有，切回主协程
// 主协程这个时候一定不会被事件阻塞，一定就会继续运行下去
//
//
static coro_sock *find_sock_by_fd(int fd)
{
    auto it = ctx.socks.find(fd);
    assert(it != ctx.socks.end());
    return it->second;
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
    if ( what & BEV_EVENT_CONNECTED ) {
        ev.event = SOCK_CONNECT_NOTIFY;
    }
    else if ( what & BEV_EVENT_ERROR ) {
        SET_ERROR(sock->status);
        ev.event = SOCK_ERROR_NOTIFY;
    }
    else if ( what & BEV_EVENT_EOF ) {
        ev.event = SOCK_EOF_NOTIFY;
        SET_EOF(sock->status);
    }
    else {
        ; // pass
    }
    sock->ctx->curev = ev;
    // TODO 
    coro_yield_uthread(ctx.io, 0);
}

// for accept event
static void sock_raw_accept_event(int s, short what, void *arg)
{
    (void)what;
    (void)s;
    global_context *ctx = (global_context *)arg;
#if 0
    int c = -1;
    struct sockaddr_storage ss;
    ev_socklen_t socklen = sizeof(ss);
    while ( true ) {
        c = accept4(s, (struct sockaddr*)&ss, &socklen, SOCK_NONBLOCK);
        if ( errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) {
            continue;
        }
        break;
    }
#endif
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
    bufferevent_disable(sock->bev, EV_READ);
    auto *buf = bufferevent_get_output(sock->bev);
    uthread_t cur = coro_current_uthread();
    size_t left = 0;
    int ret = 0;
    do {
        left = evbuffer_get_length(buf);
        if ( left > 0 ) {
            set_pending_status(sock, set_wait_write_status, cur, set_wait_write_status);
            sock->writequeue.push(cur);
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
    ssize_t status = 0;
    uthread_t cur = coro_current_uthread();
    if ( TEST_WAIT_WRITE(sock->status) ) {
        set_pending_status(sock, keep_status, cur, set_wait_write_status);
        sock->writequeue.push(cur);
        // TODO, add recv event
        status = coro_schedule_uthread(cur, 0);
    }
    int ret = status;
    if ( status >= 0 ) {
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
// 中途某次send失败了（不是第一次）
// 那么这个时候怎么处理？
ssize_t sock_send_all(int fd, char *buf, size_t size)
{
    coro_sock *sock = find_sock_by_fd(fd);
    ssize_t cnt = 0;
    ssize_t status = 0;
    uthread_t cur = coro_current_uthread();
    while ( (size_t)cnt < size ) {
        if ( status < 0 ) {
            cnt = -1;
            break;
        }
        cnt += sock_send(sock->sock, buf + cnt, size);
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
    ssize_t status = 0;
    // 调用sock_recv的时候，还没有进行yield
    // 所以，当前协程就是持有当前socket对象的协程
    // 因此，我们可以把当前协程加到socket的持有协程里面去
    uthread_t cur = coro_current_uthread();
    if ( TEST_WAIT_READ(sock->status) ) {
        // 如果当前socket在等待读
        // 那么说明当前协程已经无法继续往下执行了，
        //  1. 协程会阻塞在当前socket的读队列里面
        //  2. socket被阻塞在当前协程
        //  发生了阻塞，那么我们需要切换到其他的协程上去，放弃继续执行机会
        set_pending_status(sock, set_wait_read_status, cur, set_wait_read_status);
        sock->readqueue.push(cur);
        status = coro_schedule_uthread(cur, 0);
    }
    // 执行到这里，说明我们完成了IO操作
    // 那么说明，我们的协程当前一定没有被阻塞，所以移除阻塞标志
    ssize_t ret = status;
    if ( ret >= 0 ) {

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
    }
    return sock->sock;
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
    auto it = ctx.socks.find(fd);
    if ( it != ctx.socks.end() ) {
        coro_sock *sock = it->second;
        if ( sock->bev ) {
            bufferevent_free(sock->bev);
        }
        else {
            if ( sock->sock >= 0 ) {
                ret = close(sock->sock);
            }
        }
        ctx.socks.erase(it);
        delete sock;
    }
    return ret;
}

int sock_connect(int s_, struct sockaddr *addr, socklen_t len)
{
    (void)s_;
    event_base *base = ctx.base;
    struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    coro_sock * sock = sock_assign(-1, bev);
    // struct bufferevent *bev = bufferevent_socket_new(base, sock->sock, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, sock_buffer_read_event, sock_buffer_write_event, sock_buffer_error_event, sock);
    int ret = bufferevent_socket_connect(bev, addr, len);
    if ( ret < 0 ) {
        bufferevent_free(bev);
        delete sock;
        sock = NULL;
    }
    else {
        int s = bufferevent_getfd(bev);
        sock->sock = s;
        ctx.socks[s] = sock;

        uthread_t cur = coro_current_uthread();
        set_pending_status(sock, set_wait_connect_status, cur, set_wait_connect_status);
        sock->eventqueue.push(cur);

        ret = coro_schedule_uthread(cur, 0);

        REMOVE_WAIT_CONNECT(sock->status);

        bufferevent_enable(bev, EV_READ | EV_WRITE);
        clear_pending_status(sock, cur);
    }
    ret = -1;
    if ( sock ) {
        ret = sock->sock;
    }
    return ret;
}

static int do_accept(int sock)
{
    int c = -1;
    struct sockaddr_storage ss;
    ev_socklen_t socklen = sizeof(ss);
    while ( true ) {
        c = accept4(sock, (struct sockaddr*)&ss, &socklen, SOCK_NONBLOCK);
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

static coro_sock* sock_accept_inner(coro_sock *sock, struct sockaddr *, socklen_t *)
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
    // 因为WAIT_ACCEPT状态只有sock_accept_event_cb才能解除
    // 因此这里这里一定是sock_accept_event_cb切换回来
    // 因此，resume和yield返回值一定是sock_accept_event_cb返回的socket fd
    int ret = coro_schedule_uthread(cur, 0);
    event_del(ev);
    int s = sock->sock;
    coro_sock *client = NULL;
    if ( ret >= 0 ) {
        int c = do_accept(s);
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
    coro_sock *client = NULL;
    uthread_t cur = coro_current_uthread();
    set_pending_status(sock, set_wait_accept_status, cur, set_wait_accept_status);
    sock->eventqueue.push(cur);
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
    coro_sock *sock = find_sock_by_fd(fd);
    return listen(sock->sock, backlog);
}

int sock_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    coro_sock *sock = find_sock_by_fd(fd);
    return bind(sock->sock, addr, len);
}

int sock_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    coro_sock *sock = find_sock_by_fd(fd);
    return setsockopt(sock->sock, level, optname, optval, optlen);
}

int sock_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    coro_sock *sock = find_sock_by_fd(fd);
    return getsockopt(sock->sock, level, optname, optval, optlen);
}