#pragma once

#include <queue>
#include <deque>
#include <set>
#include <map>
#include <unordered_map>
#include <vector>
#include <atomic>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "coroutine.h"

struct coro_event_;
typedef coro_event_ coro_event;
struct coro_sock_;
typedef coro_sock_ coro_sock;
struct uthread_;
typedef uthread_ uthread;
typedef coro_event sock_event;
typedef coro_event join_event;
typedef coro_event lock_event;

struct coro_lock_;
typedef coro_lock_ coro_lock;
typedef unsigned int coro_lock_t;

typedef unsigned int uthread_t;

class global_context;

typedef void (*uthread_event_cb)(evutil_socket_t, short, void*);
typedef int(*uthread_entry)(void *);

enum io_status{
    set_wait_read_status,
    set_wait_write_status,
    set_read_status,
    set_write_status,
    set_wait_accept_status,
    set_wait_io_status,
    set_wait_connect_status,
    clear_status,
    keep_status,
};

const unsigned int READ         = 0x1;
const unsigned int WRITE        = 0x2;
const unsigned int WAIT_CONNECT = 0x4;
const unsigned int WAIT_ACCEPT  = 0x8;
const unsigned int SOCK_EOF     = 0x10;
const unsigned int SOCK_ERROR   = 0x20;
const unsigned int WAIT_JOIN    = 0x100;
const unsigned int STOP         = 0x1000;

const unsigned int CONNECTING = 0x1;
const unsigned int ACCEPTING = 0x2;
const unsigned int READING = 0x4;
const unsigned int WRITING = 0x8;

#define CLEAR_MASK(x) (x = 0)

#define TEST_EOF(x) (x & SOCK_EOF)
#define SET_ONLY_EOF(x) (x |= SOCK_EOF)
#define SET_EOF(x) ( x = SOCK_EOF )

#define TEST_ERROR(x) (x & SOCK_ERROR)
#define SET_ERROR(x) (x = SOCK_ERROR)

#define TEST_WAIT_ACCEPT(x) (x & WAIT_ACCEPT)
#define SET_WAIT_ACCEPT(x) (x |= WAIT_ACCEPT)
#define REMOVE_WAIT_ACCEPT(x) ((x &= ~WAIT_ACCEPT), SET_WRITE(x))

#define TEST_WAIT_CONNECT(x) (x & WAIT_CONNECT)
#define SET_WAIT_CONNECT(x) (x |= WAIT_CONNECT )
#define REMOVE_WAIT_CONNECT(x) ((x &= ~WAIT_CONNECT), SET_WRITE(x))

#define TEST_READ(x) (x & READ)
#define SET_READ(x) (x |= READ)

#define TEST_WAIT_READ(x) (!TEST_READ(x) && !TEST_WAIT_ACCEPT(x) && !TEST_WAIT_CONNECT(x))
#define SET_WAIT_READ(x) (x &= ~READ)
#define TEST_WAIT_READ_BUFFER(x) (!TEST_READ(x))

#define TEST_WRITE(x) (x & WRITE)
#define SET_WRITE(x) (x |= WRITE)

#define TEST_WAIT_WRITE(x) (!TEST_WRITE(x) && !TEST_WAIT_ACCEPT(x) && !TEST_WAIT_CONNECT(x))
#define SET_WAIT_WRITE(x) (x &= ~WRITE)
#define TEST_WAIT_WRITE_BUFFER(x) (!TEST_WRITE(x))

#define TEST_WAIT_IO(x) (!(TEST_WAIT_READ(x) || TEST_WAIT_WRITE(x)))
#define SET_WAIT_IO(x) (SET_WAIT_WRITE(x), SET_WAIT_READ(x))

#define TEST_STOP(x) (x & STOP)
#define SET_STOP(x) (x =| STOP)
#define REMOVE_STOP(x) (x &= ~STOP)

#define SET_CONNECTED(x) (x = WRITE)
#define SET_UNCONNECTED(x) (x &= ~WRITE)

#define TEST_WAIT_JOIN(x) (x & WAIT_JOIN)
#define SET_WAIT_JOIN(x) (x |= WAIT_JOIN)
#define REMOVE_WAIT_JOIN(x) (x &= ~WAIT_JOIN)

enum {
    NONE_EVENT,

    SOCK_CONNECT_NOTIFY,
    SOCK_ACCEPT_NOTIFY,
    SOCK_WRITE_NOTIFY,
    SOCK_READ_NOTIFY,
    SOCK_ERROR_NOTIFY,
    SOCK_EOF_NOTIFY,

    END_THREAD_NOTIFY,
    UNLOCK_NOTIFY,
};

struct coro_event_ {
    int event;
    union {
        int sock;
        uthread_t tid;
        coro_lock_t lockid; 
    };

    bool operator<(const coro_event_ &rhs) const { return sock < rhs.sock; }
};

struct uthread_ {
    coro_t * coro;                  // 协程内部实现数据结构
    coro_switcher_t *cs;            // 协程自己和调用自己的协程/线程的上下文
    uthread_t pending;              // 当前协程正在等待的目标协程
    coro_event ev;                  // 正在等待的事件
    unsigned int tid;               // 协程tid
    unsigned int status;            // 当前协程的阻塞状态
    global_context *ctx;            // 协程调度器上下文
    uthread_entry entry;            // 协程入口函数
    void *data;                     // 供用户使用，协程入口函数的参数
    void *private_data;
    free_data clean_private;        // 释放void *data
    coro_sock *pending_sock;
};

typedef std::queue<uthread_t> uthread_queue;
// typedef iterable_queue<uthread *>uthread_queue;
struct coro_sock_ {
    uthread_queue readqueue;   // 被当前sock读事件阻塞的uthreads
    uthread_queue writequeue;  // 被当前sock写事件阻塞的uthreads
    uthread_queue eventqueue;  // 被当前sock connect/accept事件阻塞的uthreads
    bufferevent *bev;          // 当前sock对象用的buffer
    size_t hwm;                // buffer高水位
    size_t lwm;                // buffer低水位
    unsigned int status;       // 当前sock的状态（可读，可写）
    int sock;                  // 当前sock对应的系统文件描述符
    global_context *ctx;       // 指向全局ctx
    void *data;                // 保留字段
    int op;
}; 
#if 0
typedef std::unordered_map<unsigned int, uthread*> uthmap;
typedef std::unordered_map<int, coro_sock*> sockmap;
typedef std::unordered_map<unsigned int, coro_lock*> lockmap;
#else
typedef std::vector<uthread*> uthmap;
typedef std::unordered_map<int, coro_sock*> sockmap;
typedef std::unordered_map<unsigned int, coro_lock*> lockmap;
#endif


struct coro_lock_ {
    coro_lock_t id;
    uthread_queue wait;
    uthread_t owner;
};

typedef coro_merge_queue<coro_event> event_merge_queue;
typedef coro_normal_queue<coro_event> event_queue;
class global_context {
public:
    uthread_t switcher;             // 调度器所在的uthread
    uthread_t io;                   // libevent io事件所在的uthread
    struct event *timer;            // 保证base loop不退出
    std::atomic_uint tid;           // 全局tid生成器引用的tid
    std::atomic_uint lockid;        // 全局lockid生成器引用的lockid
    std::atomic_uint eventid;       // 全局eventid生成器引用的eventid
    struct event_base *base;        // libevent event base
    uthread *self;                  // 当前正在运行的协程
    int cur;                        // 当前正在执行的协程的tid
    uthmap ths;                     // 所有正在执行和未完成join的线程
    sockmap socks;                  // 所有的socket对象, key是socket文件描述
    lockmap locks;                  // 所有的lock对象，key是lockid

    coro_event curev;

    // 通知所有
    void *private_data;             // 私有字段，内部自定义上下文
    void *public_data;              // 共有字段，用于用户自定义上下文

    static int schedule(void *);
    static int ioloop(void *);
    global_context();
    ~global_context();

    void release_tid(int tid);
    int alloc_tid();

    void release_lockid(int lockid);
    int alloc_lockid();
};

extern global_context ctx;
