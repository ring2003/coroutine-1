#pragma once
typedef void (*free_data)(void *);
typedef int(*uthread_entry)(void *);

struct coro_sock_;
struct uthread_;
typedef unsigned int coro_lock_t;
typedef unsigned int uthread_t;

#ifdef __cplusplus
#define EXPORT_API extern "C" 
typedef coro_sock_ coro_sock;
typedef uthread_ uthread;
#else
#define EXPORT_API
typedef struct coro_sock_ coro_sock;
typedef struct uthread_ uthread;
#endif


#define INVALID_UTHREAD ((unsigned int)-1)
