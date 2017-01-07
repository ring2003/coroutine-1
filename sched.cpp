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

// TODO, tid回收处理
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

// TODO lockid回收处理
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

    // 硬编码的最大协程，最大socks，最大locks
    ths.resize(MAX_UTHREAD, NULL);
    socks.resize(MAX_SOCKS, NULL);
    locks.resize(MAX_LOCKS, NULL);

    srand(time(NULL));

    // 主协程，其实主协程是不需要创建的，为了语义的完备性，
    // 主线程，其他线程，所以我们也引入一个主协程的定义
    // 主协程这个时候就是执行用户main函数的协程了
    // 我们创建一个空的协程，也就是0号协程，把他当做主协程
    coro_create_empty_uthread(tid);
    self = ths[tid];

    switcher = coro_create_uthread(schedule, this);
    io = coro_create_uthread(ioloop, this);

    // io协程切回主协程最少需要100ms
    // 这个时间片考虑设置得更短，但是绝大部分的场景下
    // 协程都是为了处理异步的socket io，100ms没有任何io事件似乎不大可能
    // 如果需要的话，可以把硬编码的100000改小，改成0的话，就是busy poll了
    // 空跑的情况下可能会消耗大量的cpu，这里需要有一个权衡
    // 未来这个参数可以考虑以配置或者是启动参数，或者是环境变量等方式暴露出去
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
        if ( sock->eventqueue->size() ) {
            uthread_t tid = sock->eventqueue->front();
            sock->eventqueue->pop();
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
        if ( sock->eventqueue->size() ) {
            uthread_t tid = sock->eventqueue->front();
            sock->eventqueue->pop();
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
        if ( sock->readqueue->size() ) {
            uthread_t tid = sock->readqueue->front();
            sock->readqueue->pop();
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
        if ( lock->wait->size() ) {
            uthread_t tid = lock->wait->front();
            lock->wait->pop();
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
        if ( sock->writequeue->size() ) {
            uthread_t tid = sock->writequeue->front();
            sock->writequeue->pop();
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
        shared_ptr<std::queue<uthread_t>> rq;
        shared_ptr<std::queue<uthread_t>> wq;
        wq = sock->writequeue;
        rq = sock->readqueue;
        size_t size = wq->size();
        while ( size ) {
            uthread_t tid = wq->front();
            wq->pop();
            coro_switcher_schedule_uthread(tid, -1);
            size = wq->size();
        }
        coro_sock *sock = ctx.socks[ev.sock];
        if ( sock ) {
            size = rq->size();
            size = sock->readqueue->size();
            while ( size ) {
                uthread_t tid = rq->front();
                rq->pop();
                coro_switcher_schedule_uthread(tid, -1);
                size = rq->size();
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
        shared_ptr<std::queue<uthread_t>> rq;
        shared_ptr<std::queue<uthread_t>> wq;
        wq = sock->writequeue;
        rq = sock->readqueue;
        size_t size = wq->size();
        while ( size ) {
            uthread_t tid = wq->front();
            wq->pop();
            size--;
            coro_switcher_schedule_uthread(tid, -1);
            size = wq->size();
        }
        coro_sock *sock = ctx.socks[ev.sock];
        if ( sock ) {
            size = rq->size();
            while ( size ) {
                uthread_t tid = rq->front();
                rq->pop();
                size--;
                coro_switcher_schedule_uthread(tid, -1);
                // 需要更新size，因为在其他的协程执行完毕以后，size可能改了
                size = rq->size();
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
    
    // 如果没有任何协程调度过，调度协程应该尝试唤醒io协程
    // 如果这里不唤醒io协程的话，可能所有的协程都处于io阻塞状态
    // 那么死循环执行调度协程的话，将没有任何协程有机会被调度
    // 所以必须尝试切回io协程
    // 如果io协程也没有任何io事件，因为io协程注册有一个永久生效的timer
    // （例如所有的协程都没有调用任何阻塞操作，正常执行完成）
    // 所以io协程一定会回到主协程，这个时候，就可以保证主协程一定有机会被执行
    if ( ret ) {
        coro_resume_uthread(ctx.io);
    }
}

int global_context::schedule(void *arg)
{
    global_context *ctx = (global_context *)arg;
    // 切回main协程，global_context的构造是全局构造，这个时候用户代码还没开始执行
    // 如果这里不切回主协程，那么没有机会执行用户代码了，那么调度协程会空跑
    // 而且用户协程没有机会执行了
    //  其实这个情况不会发生，因为没有任何协程被调度的话，schedule会陷入到io协程
    //  io协程会回到切回到主协程，然后再执行用户的代码，但是这样就需要等一个timeout
    coro_yield_uthread(ctx->switcher, 0);
    while ( true ) {
        // 这里不用担心死循环怎么执行其他的用户代码，因为schedule会自动切换协程
        ::schedule();
    }
    return 0;
}

int global_context::ioloop(void *arg)
{
    global_context *ctx = (global_context *)arg;
    // event_base_dispatch里面其实是libevent event_loop
    // 第一次，会在timer timeout的时候切回主协程，让调度协程工作（具体参考调度协程注释）
    // 以后，会在socket io事件或者是timer事件被触发的时候通过各种event callback函数跳出
    // 从而交出控制权
    int ret = event_base_dispatch(ctx->base);
    return ret;
}
