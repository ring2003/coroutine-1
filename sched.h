#pragma once
#include "coroutine.h"

EXPORT_API int coro_yield_uthread(uthread_t th, int yield_value);
EXPORT_API int coro_resume_value_uthread(uthread_t th, int yield_value);
EXPORT_API int coro_resume_uthread(uthread_t th);
EXPORT_API int coro_start_uthread(uthread_t th);
EXPORT_API int coro_schedule_uthread(uthread_t cur, int yield_value);
EXPORT_API int coro_switcher_schedule_uthread(uthread_t target, int yield_value);
EXPORT_API int coro_io_schedule_uthread(uthread_t target, int yield_value);
