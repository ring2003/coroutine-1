#include "internal.h"
#include <sys/mman.h>
#include <assert.h>

#ifndef PAGE_SIZE 
// TODO, use sysconf to get page size
#define PAGE_SIZE 4096
#endif

#define REDZONE PAGE_SIZE

void init_coro_stack(coro_stack *stack, size_t stacksize, coro_stack_region *region)
{
    stack->region = region;
    stack->tid = INVALID_UTHREAD;
    void *redzone = &stack->stack[stacksize];
    mprotect(redzone, REDZONE, PROT_NONE);
}

void init_coro_stack_region(coro_stack_region *region, size_t capacity, size_t stacksize)
{

    int STACKSIZE = sizeof(coro_stack) + stacksize + REDZONE;
    STACKSIZE &= ~8;
    region->used = 0; 
    region->capacity = capacity;
    region->stacks = (char *)malloc(STACKSIZE * capacity);
    region->stacksize = STACKSIZE;

    size_t i = 0;
    void *stacks = region->stacks;
    for ( i = 0; i < capacity; i++ ) {
        void *stack = &region->stacks[STACKSIZE * i];
        init_coro_stack((coro_stack*)stack, stacksize, region);
        region->free_stacks.push_front(stack);
    }
}

void *malloc_stack(coro_stack_region *region)
{
    coro_stack *stack = NULL;
    if ( region->free_stacks.size() ) {
        // 优先从front pop出来，因为这是刚刚释放回free_stacks的
        // 那么他在cpu缓存中的概率最大
        stack = (coro_stack *)*region->free_stacks.begin(); 
        region->free_stacks.pop_front();
        region->used++;
    }
    else {
        assert(0);
    }
    return stack;
}

void free_stack(coro_stack *stack)
{
    coro_stack_region *region = stack->region; 
    region->used--;
    // region->free_stacks.erase(
}
