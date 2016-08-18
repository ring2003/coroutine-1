CC = gcc
CXX = g++
LD = g++
AR = ar
CFLAGS = -DUSE_VALGRIND -Wno-unused-function -O2 -g -Wall -c -Wno-attributes -fPIC -shared

all: testsock liblxsock.a liblxsock.so

coro.o: coro.c coro.h
	$(CC) $(CFLAGS) $< -o $@

test.o: test.cpp coro.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

sock.o : sock.cpp internal.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

thread.o : thread.cpp internal.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

manager.o : manager.cpp internal.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

util.o : util.cpp
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

sched.o : sched.cpp internal.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

lock.o : lock.cpp internal.h
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@

testsock: coro.o sock.o test.o util.o sched.o manager.o thread.o lock.o
	$(CXX) -g -O2 -Wall $^ -o $@ -levent

liblxsock.a: coro.o sock.o util.o sched.o manager.o thread.o lock.o
	$(AR) rcs $@ $^

liblxsock.so: coro.o sock.o util.o sched.o manager.o thread.o lock.o
	$(CXX) -g -shared  $^ -o $@

clean:
	rm -f *.o vgcore.* core core.* testsock liblxsock.a liblxsock.so
