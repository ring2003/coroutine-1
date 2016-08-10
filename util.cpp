#include "util.h"

// do_schedule 需要改进
//
// 我们约定，
// 1. 在sock_$event_cb里面进行pending队列的pop操作
// 2. 在sock_$op里面进行pending队列的push操作
uthread_t do_schedule(uthread_queue &queue)
{
    uthread_t ret = -1;
    if ( queue.size() ) {
        ret = queue.front();
        queue.pop();
    }
    return ret;
}
