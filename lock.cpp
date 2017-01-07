#include "internal.h"
#include "lock.h"

int coro_uthread_mutex_init(coro_lock_t* lock)
{
    *lock = ctx.alloc_lockid();
    coro_lock *l = new coro_lock;
    l->id = *lock;
    l->owner = INVALID_UTHREAD;
    ctx.locks[*lock] = l;
    l->wait = std::make_shared<std::queue<uthread_t>>();
    return 0; 
}

int coro_uthread_mutex_lock(coro_lock_t *l)
{
    int ret = -1;
    uthread_t cur = coro_current_uthread();
    coro_lock_t lock_ = *l;
    coro_lock * lock = ctx.locks[lock_];
    if ( lock ) {
        if ( lock->owner != INVALID_UTHREAD ) {
            lock->wait->push(cur);
        }
        while ( true ) {
            // 这个和多次join同一个协程的情况一样
            // 其实这种行为也是不允许的，在还有
            // 协程再等待mutex的时候，mutex被销毁了
            // 这种行为同样是未定义的
            // 但是为了防止用户真的这么做的，还是防止崩溃比较好
            if ( !lock ) {
                break;
            }
            if ( lock->owner == INVALID_UTHREAD ) {
                lock->owner = cur;
                ret = 0;
                break;
            }
            else {
                coro_schedule_uthread(cur, 0);
                lock = ctx.locks[lock_];
            }
        }
    }
    return ret;
}

int coro_uthread_mutex_unlock(coro_lock_t *l)
{
    int ret = -1;
    coro_lock_t lock_ = *l;
    auto lock = ctx.locks[lock_];
    if ( lock ) {
        if ( lock->owner == coro_current_uthread() ) {
            ret = 0;
            lock->owner = INVALID_UTHREAD;
            coro_event ev;
            ev.event = UNLOCK_NOTIFY;
            ev.lockid = *l;
            ctx.curev = ev;
        }
    }
    return ret;
}

int coro_uthread_mutex_release(coro_lock_t *l)
{
    int ret = -1;
    coro_lock_t lock_ = *l;
    coro_lock * lock = ctx.locks[lock_];
    if ( lock ) {
        if ( lock->owner == INVALID_UTHREAD ) {
            ctx.locks[lock_] = NULL;
            ret = 0;
        }
        delete lock;
    }
    return ret;
}
