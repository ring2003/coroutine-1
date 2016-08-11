#include "internal.h"
#include "lock.h"

int coro_uthread_mutex_init(coro_lock_t* lock)
{
    *lock = ctx.alloc_lockid();
    coro_lock *l = new coro_lock;
    l->id = *lock;
    l->owner = INVALID_UTHREAD;
    ctx.locks[*lock] = l;
    return 0; 
}

int coro_uthread_mutex_lock(coro_lock_t *l)
{
    int ret = -1;
    uthread_t cur = coro_current_uthread();
    coro_lock_t lock = *l;
    auto it = ctx.locks.find(lock);
    if ( it != ctx.locks.end() ) {
        coro_lock *lock = it->second;
        if ( lock->owner != INVALID_UTHREAD ) {
            lock->wait.push(cur);
        }
        while ( true ) {
            if ( lock->owner == INVALID_UTHREAD ) {
                lock->owner = cur;
                ret = 0;
                break;
            }
            else {
                coro_schedule_uthread(cur, 0);
            }
        }
    }
    return ret;
}

int coro_uthread_mutex_unlock(coro_lock_t *l)
{
    int ret = -1;
    coro_lock_t lock = *l;
    auto it = ctx.locks.find(lock);
    if ( it != ctx.locks.end() ) {
        coro_lock *lock = it->second;
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
    coro_lock_t lock = *l;
    auto it = ctx.locks.find(lock);
    if ( it != ctx.locks.end() ) {
        auto lockobj = it->second;
        if ( it->second->owner == INVALID_UTHREAD ) {
            ctx.locks.erase(it);
            ret = 0;
        }
        delete lockobj;
    }
    return ret;
}
