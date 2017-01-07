#pragma once
#include "coroutine.h"
EXPORT_API void coro_set_currentid(int tid);
EXPORT_API int coro_current_tid_uthread();
EXPORT_API void* coro_uthread_get_data(uthread_t th);
EXPORT_API void* coro_uthread_set_data(uthread_t th, void *data, free_data cb);
EXPORT_API uthread_t coro_current_uthread();
EXPORT_API bool is_main_by_uthread(uthread_t th);
EXPORT_API uthread_t coro_create_uthread(uthread_entry cb, void *data);
EXPORT_API uthread_t coro_create_empty_uthread(int tid);
EXPORT_API void coro_uthread_free(uthread_t tid);
EXPORT_API int coro_join_uthread(uthread_t tid);
EXPORT_API int coro_sleep(int ms);
EXPORT_API int coro_start_uthread(uthread_t th);
EXPORT_API int coro_tid_uthread(uthread_t th);
