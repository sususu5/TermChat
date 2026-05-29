#pragma once

#include <assert.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

class Epoller {
public:
    explicit Epoller(int maxEvent = 4096);
    ~Epoller();

    bool addFd(int fd, uint32_t events);
    bool modFd(int fd, uint32_t events);
    bool delFd(int fd);
    int Wait(int timeoutMs = -1);
    int getEventFd(size_t i) const;
    uint32_t getEvents(size_t i) const;

private:
    // The file descriptor of the epoll instance
    int epollFd_;
    std::vector<struct epoll_event> events_;
};
