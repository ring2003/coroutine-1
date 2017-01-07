#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <evutil.h>

#include "internal.h"
#include "coroutine.h"
#include "util.h"

#define MAX_UTHREAD 3000000
#define MAX_SOCKS 65535
#define MAX_LOCKS 65535

global_context ctx;

// TODO, tid循环
int global_context::alloc_tid()
{
    ++tid;
    // if ( tid == (unsigned int)(-1) ) {
    if ( tid == MAX_UTHREAD ) {
        assert(0);
    }
    return tid;
}

void global_context::release_tid(int tid)
{
    // ths.erase(ths.find(tid));
}

int global_context::alloc_lockid()
{
    ++lockid;
    if ( lockid == (unsigned)(-1) ) {
        assert(0);
    }
    return lockid;
}

void global_context::release_lockid(int lockid)
{
    locks[lockid] = NULL;
}

static void io_back_to_main(int, short what, void *arg)
{
    (void)arg;
    if ( what & EV_TIMEOUT ) {
        coro_yield_uthread(ctx.io, 0);
    }
}

global_context::global_context()
{ 
    tid = 0;
    lockid = 0;
    cur = 0;
    private_data = NULL;
    base = event_base_new();

    ths.resize(MAX_UTHREAD, NULL);
    socks.resize(MAX_SOCKS, NULL);
    locks.resize(MAX_LOCKS, NULL);

    srand(time(NULL));

    coro_create_empty_uthread(tid);
    self = ths[tid];

    switcher = coro_create_uthread(schedule, this);
    io = coro_create_uthread(ioloop, this);

    struct timeval tv;
    tv.tv_sec = 0;
    // 100ms
    tv.tv_usec = 100000;

    timer = event_new(base, -1, EV_PERSIST, io_back_to_main, NULL);
    evtimer_add(timer, &tv);

    coro_resume_uthread(io);
    coro_resume_uthread(switcher);
}

global_context::~global_context()
{
    // 当我们在某次event callback回调中回到主线程的话
    // 我们bufferevent可能持有一个引用，还有解引用
    // 如果我们直接退出，那么可能导致无法释放某些对象
    // 虽然进程退出以后，内存会清理，但是为了完备性，我们还是切换一次，保证内存正常释放
    coro_resume_uthread(io);
    event_del(timer);
    event_free(timer);
    coro_uthread_free(self->tid);
    coro_uthread_free(io);
    coro_uthread_free(switcher);
    event_base_free(base);
}

int accept_schedule(coro_event ev)
{
    int ret = -1;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        if ( sock->eventqueue.size() ) {
            uthread_t tid = sock->eventqueue.front();
            sock->eventqueue.pop();
            coro_switcher_schedule_uthread(tid, 0);
            ret = 0;
        }
    }
    return ret;
}

int connect_schedule(coro_event ev)
{
    int ret = -1;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        if ( sock->eventqueue.size() ) {
            uthread_t tid = sock->eventqueue.front();
            sock->eventqueue.pop();
            coro_switcher_schedule_uthread(tid, 0);
            ret = 0;
        }
    }
    return ret;
}

int read_schedule(coro_event ev)
{
    int ret = -1;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        SET_READ(sock->status);
        if ( sock->readqueue.size() ) {
            uthread_t tid = sock->readqueue.front();
            sock->readqueue.pop();
            coro_switcher_schedule_uthread(tid, 0);
            ret = 0;
        }
    }
    return ret;
}

int unlock_schedule(coro_event ev)
{
    int ret = -1;
    coro_lock *lock = ctx.locks[ev.lockid];
    if ( lock ) {
        if ( lock->wait.size() ) {
            uthread_t tid = lock->wait.front();
            lock->wait.pop();
            coro_switcher_schedule_uthread(tid, 0);
            ret = 0;
        }
    }
    return ret;
}

int write_schedule(coro_event ev)
{
    int ret = -1;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        SET_WRITE(sock->status);
        if ( sock->writequeue.size() ) {
            uthread_t tid = sock->writequeue.front();
            sock->writequeue.pop();
            coro_switcher_schedule_uthread(tid, 0);
            ret = 0;
        }
    }
    return ret;
}

int error_schedule(coro_event ev)
{
    (void)ev;
    int ret = 0;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        size_t size = sock->writequeue.size();
        while ( size ) {
            uthread_t tid = sock->writequeue.front();
            sock->writequeue.pop();
            coro_switcher_schedule_uthread(tid, -1);
            sock = ctx.socks[ev.sock];
            if ( !sock ) {
                break;
            }
        }
        coro_sock *sock = ctx.socks[ev.sock];
        if ( sock ) {
            size = sock->readqueue.size();
            while ( size ) {
                uthread_t tid = sock->readqueue.front();
                sock->readqueue.pop();
                coro_switcher_schedule_uthread(tid, -1);
                // after schedule, sock maybe closed!
                sock = ctx.socks[ev.sock];
                if ( !sock ) {
                    break;
                }
            }
        }
    }
    return ret;
}

int eof_schedule(coro_event ev)
{
    int ret = 0;
    coro_sock *sock = ctx.socks[ev.sock];
    if ( sock ) {
        size_t size = sock->writequeue.size();
        while ( size ) {
            uthread_t tid = sock->writequeue.front();
            sock->writequeue.pop();
            size--;
            coro_switcher_schedule_uthread(tid, -1);
            sock = ctx.socks[ev.sock];
            if ( !sock ) {
                break;
            }
        }
        coro_sock *sock = ctx.socks[ev.sock];
        if ( sock ) {
            size = sock->readqueue.size();
            while ( size ) {
                uthread_t tid = sock->readqueue.front();
                sock->readqueue.pop();
                size--;
                coro_switcher_schedule_uthread(tid, -1);
                sock = ctx.socks[ev.sock];
                if ( !sock ) {
                    break;
                }
            }
        }
    }
    return ret;
}

int join_schedule(coro_event ev)
{
    int ret = -1;
    //uthread * th = ctx.ths.find(ev.tid)->second;
    uthread * th = ctx.ths[ev.tid];
    if ( th ) {
        if ( th->pending != INVALID_UTHREAD ) {
            ret = 0;
            coro_switcher_schedule_uthread(th->pending, 0);
        }
    }
    return ret;
}

static void exit_schedule()
{
}

static void schedule()
{
    int ret = -1;
    coro_event ev = ctx.curev;
    if ( ev.event != NONE_EVENT ) {
        ctx.curev = {NONE_EVENT, {-1}};
        if ( ev.event == END_THREAD_NOTIFY ) {
            ret = join_schedule(ev);
        }
        else if ( ev.event == SOCK_ACCEPT_NOTIFY ) {
            ret = accept_schedule(ev);
        }
        else if ( ev.event == SOCK_CONNECT_NOTIFY ) {
            ret = connect_schedule(ev);
        }
        else if ( ev.event == SOCK_ERROR_NOTIFY ) {
            ret = error_schedule(ev);
        }
        else if ( ev.event == SOCK_EOF_NOTIFY ) {
            ret = eof_schedule(ev);
        }
        else if ( ev.event == SOCK_READ_NOTIFY ) {
            ret = read_schedule(ev);
        }
        else if ( ev.event == SOCK_WRITE_NOTIFY ) {
            ret = write_schedule(ev);
        }
        else if ( ev.event == UNLOCK_NOTIFY ) {
            ret = unlock_schedule(ev);
        }
        else {
        }
    }
    
    if ( ret ) {
        coro_resume_uthread(ctx.io);
    }
}

int global_context::schedule(void *arg)
{
    global_context *ctx = (global_context *)arg;
    coro_yield_uthread(ctx->switcher, 0);
    while ( true ) {
        ::schedule();
    }
    return 0;
}

int global_context::ioloop(void *arg)
{
    global_context *ctx = (global_context *)arg;
    int ret = event_base_dispatch(ctx->base);
    return ret;
}
