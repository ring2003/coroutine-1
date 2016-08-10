#include "internal.h"
#include "sock.h"
#include "thread.h"
#include "sched.h"
// yield 和 resume都会导致context swap
// 都会主动触发协程的切换，因此我们要保存好自己的tid(协程id)
// 当切换回来以后我们再设置回来
// 那么，只要保证运行一个协程的时候，我们设置好了当前的tid
// 那么我们的ctx.cur就永远是当前的正在运行的协程tid
// 参考entry_point函数的实现，设置当前ctx.cur
int coro_yield_uthread(uthread_t th, int yield_value)
{
#ifdef SWITCH_DEBUG
    printf("yield begin current id: %d, target: %d\n", ctx.cur, th->tid);
#endif
    int old_tid = ctx.cur;
    int ret = coro_yield(ctx.ths[th]->coro, yield_value);
    ctx.cur = old_tid;
#ifdef SWITCH_DEBUG
    printf("yield end current: %d from: %d\n", ctx.cur, th->tid);
#endif
    return ret;

}

int coro_resume_value_uthread(uthread_t th, int yield_value)
{
#ifdef SWITCH_DEBUG
    printf("resume begin: current id: %d target: %d\n", ctx.cur, th->tid);
#endif
    int old_tid = ctx.cur;
    int ret = coro_resume_value(ctx.ths[th]->coro, yield_value);
    ctx.cur = old_tid;
#ifdef SWITCH_DEBUG
    printf("resume end: current: %d from id: %d\n", ctx.cur, th->tid);
#endif
    return ret;
}

int coro_resume_uthread(uthread_t th)
{
    int old_tid = ctx.cur;
#ifdef SWITCH_DEBUG
    printf("resume begin: current id: %d target: %d\n", ctx.cur, th->tid);
#endif
    int ret = coro_resume(ctx.ths[th]->coro);
#ifdef SWITCH_DEBUG
    printf("resume end: back from id: %d\n", th->tid);
#endif
    ctx.cur = old_tid;
    return ret;
}

int coro_start_uthread(uthread_t th)
{
    return coro_resume_uthread(th);
}

// 此函数禁止在IO协程上下文调用，例如各种event事件
// 因为IO协程需要yield回主线程，resume其他协程
// 和上述的过程刚好相反！
int coro_schedule_uthread(uthread_t cur, int yield_value)
{
    (void)yield_value;
    bool bmain = is_main_by_uthread(cur);
    int ret = -1;
    if ( bmain) {
        ret = coro_resume_uthread(ctx.switcher);
    }
    else {
        ret = coro_yield_uthread(cur, yield_value);
    }
    return ret;
}

int coro_switcher_schedule_uthread(uthread_t target, int yield_value)
{
    bool bmain = is_main_by_uthread(target);
    uthread_t cur = coro_current_uthread();
    int ret = -1;
    if ( bmain ) {
        ret = coro_yield_uthread(cur, yield_value);
    }
    else {
        ret = coro_resume_value_uthread(target, yield_value);
    }
    return ret;
}

// 参见coro_shedule_uthread
// 该函数只能在IO协程上下文调用，也就是各种event事件
int coro_io_schedule_uthread(uthread_t target, int yield_value)
{
    bool bmain = is_main_by_uthread(target);
    uthread_t cur = coro_current_uthread();
    int ret = -1;
    if ( bmain ) {
        ret = coro_yield_uthread(cur, yield_value);
    }
    else {
        ret = coro_resume_value_uthread(target, yield_value);
    }
    return ret;
}


