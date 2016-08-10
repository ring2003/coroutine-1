#pragma once
#include <queue>
#include "common.h"
typedef std::queue<uthread_t> uthread_queue;
EXPORT_API uthread_t do_schedule(uthread_queue &queue);
