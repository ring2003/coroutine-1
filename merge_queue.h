#pragma once
#include <queue>
#include <set>

template<typename T, typename Container=std::deque<T> >
class coro_queue_inner : public std::queue<T, Container>
{
public:
    typedef typename Container::iterator iterator;
    typedef typename Container::const_iterator const_iterator;

    iterator begin() { return this->c.begin(); }
    iterator end() { return this->c.end(); }
    const_iterator begin() const { return this->c.begin(); }
    const_iterator end() const { return this->c.end(); }
};

template <typename T, bool flag>
class coro_queue {
};

template <typename T>
class coro_queue<T, false> {
private:
    coro_queue_inner<T> queue;
public:
    typedef typename coro_queue_inner<T>::iterator iterator;
    typedef typename coro_queue_inner<T>::const_iterator const_iterator;
    void pop() { return queue.pop(); }
    T front() { return queue.front(); }
    void push(const T& o) { return queue.push(o); }
    void push(const T&& o) { return queue.push(std::forward(o)); }
    unsigned size() { return queue.size(); }
    bool empty() { return queue.empty(); }
    iterator begin() { return queue.begin(); }
    iterator end() { return queue.end(); }
    const_iterator cbegin() { return queue.cbegin(); }
    const_iterator cend() { return queue.cend(); }
};

template <typename T>
class coro_queue<T, true> {
private:
    coro_queue_inner<T> queue;
    std::set<T> set;
public:
    typedef typename coro_queue_inner<T>::iterator iterator;
    typedef typename coro_queue_inner<T>::const_iterator const_iterator;
    void pop()
    { 
        auto &o = queue.front();
        set.erase(o);
        queue.pop();
    }
    T front() { return queue.front(); }
    void push(const T& o)
    { 
        if ( set.find(o) == set.end() ) {
            return queue.push(o);
        }
    }
    void push(const T&& o)
    { 
        if ( set.find(o) == set.end() ) {
            return queue.push(std::forward(o));
        }
    }
    unsigned size() { return queue.size(); }
    bool empty() { return queue.empty(); }
    iterator begin() { return queue.begin(); }
    iterator end() { return queue.end(); }
    const_iterator cbegin() { return queue.cbegin(); }
    const_iterator cend() { return queue.cend(); }
};


template <typename T>
using coro_merge_queue = coro_queue<T, true>;

template <typename T>
using coro_normal_queue = coro_queue<T, false>;
