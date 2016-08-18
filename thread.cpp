#include "internal.h"
#include "thread.h"

void coro_set_currentid(int tid)
{
    ctx.cur = tid;
}

int coro_current_tid_uthread()
{
    return ctx.cur;
}

void* coro_uthread_get_data(uthread_t th)
{
    return ctx.ths[th]->private_data;
}

void* coro_uthread_set_data(uthread_t th, void *data, free_data cb)
{
    auto thread = ctx.ths[th];
    void *old = thread->private_data;
    thread->private_data = data;
    thread->clean_private = cb;
    return old;
}

int coro_tid_uthread(uthread_t th)
{
    return th;
}

uthread_t coro_current_uthread()
{
    return ctx.cur;
}

bool is_main_by_uthread(uthread_t th)
{
    return th == 0;
}

uthread_t coro_create_empty_uthread(int tid)
{
    uthread *th = new uthread;
    th->data = NULL;
    th->private_data = NULL;
    th->coro = NULL;
    th->ctx = &ctx;
    th->cs = NULL;
    th->tid = tid;
    th->status = 0;
    th->entry = NULL;
    ctx.ths[th->tid] = th;
    return th->tid;
}

static int entry_point(coro_t *coro)
{
    uthread *th = (uthread *)coro_get_data(coro);
    int old_tid = ctx.cur;
    ctx.cur = th->tid;
    int ret = th->entry(th->data);
    ctx.cur = old_tid;
    th->status = STOP;
    coro_event e;
    e.event = END_THREAD_NOTIFY;
    e.tid = th->tid;
    ctx.curev = e;
    return ret;
}

uthread_t coro_create_uthread(uthread_entry cb, void *data)
{
    coro_switcher_t *cs = new coro_switcher_t;
    uthread *th = new uthread;
    th->data = data;
    th->private_data = NULL;
    coro_t *coro = coro_new(cs, entry_point, th);
    th->coro = coro;
    th->ctx = &ctx;
    th->cs = cs;
    th->entry = cb;
    th->tid = ctx.alloc_tid();
    th->status = 0;
    th->clean_private = NULL;
    ctx.ths[th->tid] = th;
    th->pending = (unsigned)-1;
    return th->tid;
}

void coro_uthread_free(uthread_t tid)
{
    uthread *th = ctx.ths[tid];
    if ( th->private_data && th->clean_private ) {
        th->clean_private(th->private_data);
    }
    if ( th->coro ) {
        coro_free(th->coro);
    }
    if ( th->cs ) {
        delete th->cs;
    }
    delete th;
    ctx.ths[tid] = NULL;
}

// 多个协程同时join一个线程是未定义行为
// 这个和pthread_join一致
int coro_join_uthread(uthread_t tid)
{
    int ret = -1;
    auto th = ctx.ths[tid];
    if ( th != NULL ) {
        uthread_t tid = th->tid;
        uthread_t cur = coro_current_uthread(); 
        while ( true ) {
            th->pending = cur;
            if ( TEST_STOP(th->status) ) {
                coro_uthread_free(tid);
                ret = 0;
                break;
            }
            coro_schedule_uthread(cur, 0);
        }
    }
    return ret;
}

typedef struct timer_ctx_ {
    struct event *ev;
    uthread_t th;
}timer_ctx;

static void once_timer_cb(int, short what, void *arg)
{
    timer_ctx *timer_context = (timer_ctx *)arg;
    if ( what & EV_TIMEOUT ) {
        event_del(timer_context->ev);
        event_free(timer_context->ev);
        coro_io_schedule_uthread(timer_context->th, 0);
    }
}

static timeval ms2timeval(int ms)
{
    int sec;
    int usec;
    if ( ms > 1000 ) {
        sec = ms / 1000;
        ms = ms % 1000;
        usec = ms * 1000;
    }
    else {
        sec = 0;
        usec =  ms * 1000;
    }
    struct timeval tv = {sec, usec};
    return tv;
}

int coro_sleep(int ms)
{
    uthread_t cur = coro_current_uthread();
    struct timeval tv = ms2timeval(ms);
    timer_ctx timer_context;
    // 因为下面会yield或者resume出去，在调用once_timer_cb的时候
    // timer_context所在的栈上内存一定没有自动销毁
    // 普通函数不能这样做！
    struct event *once_timer = event_new(ctx.base, -1, EV_PERSIST, once_timer_cb, &timer_context);
    timer_context.ev = once_timer;
    timer_context.th = cur;
    evtimer_add(once_timer, &tv);
    coro_schedule_uthread(cur, 0);
    return 0;    
}

