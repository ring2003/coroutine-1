#include "internal.h"
#include "sock.h"
#include "thread.h"
#include "coro_context.h"
// yield 和 resume都会导致context swap
// 都会主动触发协程的切换，因此我们要保存好自己的tid(协程id)
// 当切换回来以后我们再设置回来
// 那么，只要保证运行一个协程的时候，我们设置好了当前的tid
// 那么我们的ctx.cur就永远是当前的正在运行的协程tid
// 参考entry_point函数的实现，设置当前ctx.cur
int coro_yield_uthread(uthread_t th, int yield_value)
{
#ifdef SWITCH_DEBUG
    printf("yield begin current id: %d, target: %d\n", ctx.cur, th);
#endif
    int old_tid = ctx.cur;
    int ret = coro_yield(ctx.ths[th]->coro, yield_value);
    ctx.cur = old_tid;
#ifdef SWITCH_DEBUG
    printf("yield end current: %d from: %d\n", ctx.cur, th);
#endif
    return ret;

}

int coro_resume_value_uthread(uthread_t th, int yield_value)
{
#ifdef SWITCH_DEBUG
    printf("resume begin: current id: %d target: %d\n", ctx.cur, th);
#endif
    int old_tid = ctx.cur;
    int ret = coro_resume_value(ctx.ths[th]->coro, yield_value);
    ctx.cur = old_tid;
#ifdef SWITCH_DEBUG
    printf("resume end: current: %d from id: %d\n", ctx.cur, th);
#endif
    return ret;
}

int coro_resume_uthread(uthread_t th)
{
    int old_tid = ctx.cur;
#ifdef SWITCH_DEBUG
    printf("resume begin: current id: %d target: %d\n", ctx.cur, th);
#endif
    int ret = coro_resume(ctx.ths[th]->coro);
#ifdef SWITCH_DEBUG
    printf("resume end: back from id: %d\n", th);
#endif
    ctx.cur = old_tid;
    return ret;
}

int coro_start_uthread(uthread_t th)
{
    return coro_resume_uthread(th);
}

// 此函数禁止在IO, switch协程上下文调用，例如各种event事件
// 主协程和用户协程在调用过程中，需要唤醒switch或者是parent协程（最终唤醒switch）来触发调度
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

// 该函数只能在switch协程上下文调用
// switch协程负责接收io协程或者是其他的协程抛过来的具体的event来触发调度规则
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

// 该函数只能在IO协程上下文调用，也就是各种event事件
// IO协程需要yield回主线程，然后通过sock_event_* 或者是timer事件回到主协程
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


