#pragma once
#include "common.h"
EXPORT_API int coro_uthread_mutex_init(coro_lock_t* lock);
EXPORT_API int coro_uthread_mutex_lock(coro_lock_t* l);
EXPORT_API int coro_uthread_mutex_unlock(coro_lock_t* l);
EXPORT_API int coro_uthread_mutex_release(coro_lock_t* l);
