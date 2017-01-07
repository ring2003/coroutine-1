### A Simple Project likes Gevent For c++

Base in Libevent and lwan's coroutine

# Need lwan testrunner started at 127.0.0.1:8080 (default)

./main
=======
### 关于coro这个coroutine实现
* coroutine的实现有一个很麻烦的问题，就是函数的堆栈是固定，没有办法做到栈自动增长，os线的的栈是可以自动增长的。可以考虑在多分配一个页面作为函数栈，然后这个页面作为redzone，也就是设置这个页面不可读写，但是这样其实也保证不了大范围的越界，所以这里只能尽可能避免太深的栈调用和栈上大数组分配了。
* 另外一个就是，coro的context切换是没有走glibc的context swap，因为这样会带来大量的cs开销，因为这是一个系统调用。但是，需要注意的是，coro的context swap并没有保存浮点寄存器，比如XMM之类的，所以，不是所有的场合都可以用的
* crt里面有部分的函数是不允许使用的，比如strtok，gethostbyname之类的函数，他们内部有static变量，coroutine的实现，在一个线程上会执行多个coroutine，但是他们共享了一个static变量，就导致了这些C函数的不可重入，不过线程情况下，这个情况是一样的。但是，更加严重的是，tls变量，tls变量是线程局部共享的，在线程模型里面，是隔离的，但是在corotine上下文，就有问题，导致不可重入。
* coroutine函数栈最好不要随意分配，最好是排列在一块连续的内存区域，然后维护一个专门的栈内存queue，用来写gdb辅助脚本调试，否则跨函数栈调试coroutine太麻烦，当然这个是debug功能


### golang模型是GMP
* 不能有join，因为coroutine完成以后，会被调度器拿去作为其他task的载体，因此需要做一个类似defer的功能。
* 支持不同的P之间偷任务，那么，任务队列需要加锁，一个P代表一个OS thread。这种情况下，3个选择，一个是lock-free queue，一个是加锁，还一个选择是学pypy走stm，当然可以考虑HTM，但是HTM的cpu支持是问题。
* coroutine唤醒的mutex，mutex目前是轻量级的实现，只是一个标志位，在单线程情况下，没有问题。在多线程的情况下，需要膨胀，可以参考java的锁膨胀策略
* 调度器优化，可能需要写一个公平调度。目前的调度模型，一个io coroutine，一个switch coroutine，一个main coroutine，io和switch 的parent是main，约定io和switch协程不允许创建协程，只有main和用户自定义的协程可以创建协程，每次调度，一旦有事件阻塞（sleep，lock，socket block...），立马切换到自己的parent协程，然后parent协程再发生阻塞，再切换到自己的父协程，直到最后一定会切换到main协程，main协程一直往下执行，直到main协程阻塞，他会切换到switch协程，然后switch协程陷入调度死循环，如果有协程被调度过，继续等待下一次调度，否则switch协程等待阻塞事件，唤醒io协程，等待io事件的死循环。
因此，这个模型其实是一个回溯执行的过程，做不到公平调度，在某些特殊情况下，可能会存在饥渴。
之所以采用这个设计还有一个原因，这个设计可以保证，总能回到主协程，然后主协程退出以后，如果main函数结束了，那么就等于退出了所有协程，模拟了主线程退出整个进程退出的情况

### 协程调度模型：
初始化的过程是这样子的，首先main协程启动io协程，然后io协程有个定时器，切回main协程，然后main协程启动switch协程，switch协程立马切回main，这个时候还没进入switch的调度死循环，然后，main协程完成了全局初始化，开始执行用户代码。io协程的定时器必须是永久定时器，否则可能出现这样的情况，某个时刻，main协程处理某个join事件，开始进入阻塞，然后切换到switch协程，不巧的是，switch协程调度的过程中，所有的其他协程，都没有发生任何的阻塞事件，自然退出了协程，这个时候，switch协程就会陷入到io携程中（因为发生了调度，所以一定会切换到io协程），然而io协程没有io事件触发他了，所以就永远不会切回到调度协程（io协程通过各种event切回到其他协程，但是这个时候没有io event了，就不可能切了），那么就变成了libevent空跑的状态了。

* init: 
>
  main -> io（timer back to main) -> main -> switch (coro_yield_uthread back to main) -> main -> user code

* running: 
>
 usercode -> switch(busy loop) -> io(event loop call sock event* back to usercode) -> usercode -> ....

### 高低水位没有使用:
sock里面的hwm和lwm本意是作为send的buffer的控制的，目前没有使用
原本设计是这个字段是作为libevent的highwatermark设计的，这样可以防止内存暴涨，不设置highwatermark的话，内核不接受的send buffer会被一直缓存在libevent内部，可能导致内存暴涨
