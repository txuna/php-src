#include <stdio.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/prctl.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <time.h>
#include "zend.h"
#include "efpm.h"
#include "efpm_event.h"

struct efpm_event_module_s *new_event_module(int max) {
    if(max <= 0){
        return NULL;
    }

    struct efpm_event_module_s *module = (struct efpm_event_module_s*)malloc(sizeof(struct efpm_event_module_s));
    if(!module) {
        return NULL;
    }

    module->nepollfds = max;
    module->epollfd = -1;
    module->epollfds = NULL;
    module->queue = NULL;

    module->init = efpm_event_init_main;
    module->clean = efpm_event_clean;
    module->wait = efpm_event_wait;
    module->add = efpm_event_add;
    module->remove = efpm_event_remove;
    module->fire = efpm_event_fire;

    return module;
}

int efpm_event_init_main(struct efpm_event_module_s *this) {
    this->epollfd = epoll_create(this->nepollfds);
    if(this->epollfd < 0){
        printf("failed to epoll_create(): %d\n", this->epollfd);
        return FAILURE;
    }

    this->epollfds = malloc(sizeof(struct epoll_event) * this->nepollfds);
    if(!this->epollfds){
        printf("failed to allocate epollfds\n");
        return FAILURE;
    }

    memset(this->epollfds, 0, sizeof(struct epoll_event) * this->nepollfds);
    return SUCCESS;
}

int efpm_event_clean(struct efpm_event_module_s *this) {
    struct efpm_event_queue_s *tmp = this->queue;
    struct efpm_event_queue_s *prev = tmp;
    while(tmp) {
        struct epoll_event e;
        e.data.ptr = tmp->ev;
        if(epoll_ctl(this->epollfd, EPOLL_CTL_DEL, tmp->ev->fd, &e) == -1){
            printf("epoll_ctl(): %d\n", errno);
        }

        prev = tmp;
        tmp = tmp->next;
        free(prev);
    }

    this->queue = NULL;

    if(this->epollfds) {
        free(this->epollfds);
        this->epollfds = NULL;
    }

    if(this->epollfd != -1){
        close(this->epollfd);
        this->epollfd = -1;
    }

    this->nepollfds = 0;
    return SUCCESS;
}

#define CLOCK_MONOTONIC 1

int new_timerfd(int sec, int milli) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    struct itimerspec its;
    its.it_value.tv_sec = sec;
    its.it_value.tv_nsec = milli * 1000000;
    its.it_interval.tv_sec = sec;
    its.it_interval.tv_nsec = milli * 1000000;

    if(timerfd_settime(tfd, 0, &its, NULL) < 0) {
        printf("timer_settime(): %d\n", errno);
        return FAILURE;
    }

    return tfd;
}

struct efpm_event_s *efpm_event_set(int fd, void (*callback)(struct efpm_event_s *, uint32_t flags, void *arg), void *arg) {
    struct efpm_event_s *ev = (struct efpm_event_s *)malloc(sizeof(struct efpm_event_s));
    if(!ev){
        return NULL;
    }

    memset(ev, 0, sizeof(struct efpm_event_s));

    ev->fd = fd;
    ev->arg = arg; // this;
    ev->callback = callback;

    return ev;
}

int efpm_event_add(struct efpm_event_module_s *this, struct efpm_event_s *ev){
    struct epoll_event e;
    e.events = EPOLLIN;
    e.data.ptr = ev;

    // level trigger
    if(epoll_ctl(this->epollfd, EPOLL_CTL_ADD, ev->fd, &e) == -1){
        printf("epoll_ctl(): %d\n", errno);
        return FAILURE;
    } 

    ev->index = ev->fd;

    // queue insert
    if(!this->queue){
        this->queue = (struct efpm_event_queue_s*)malloc(sizeof(struct efpm_event_queue_s));
        this->queue->next = NULL;
        this->queue->ev = ev;
    } else {
        struct efpm_event_queue_s *tmp = this->queue;
        struct efpm_event_queue_s *prev = tmp;
        while(tmp) {
            prev = tmp;
            tmp = tmp->next;
        }
        tmp = (struct efpm_event_queue_s*)malloc(sizeof(struct efpm_event_queue_s));
        tmp->next = NULL;
        tmp->ev = ev;
        prev->next = tmp;
    }
    
    return SUCCESS;
}

int efpm_event_remove(struct efpm_event_module_s *this, struct efpm_event_s *ev) {
    struct epoll_event e;

    e.events = EPOLLIN;
    e.data.ptr = ev;

    if(epoll_ctl(this->epollfd, EPOLL_CTL_DEL, ev->fd, NULL) == -1){
        printf("epoll_ctl(): %d\n", errno);
        return FAILURE;
    }

    ev->index = -1;

    // queue delete
    if(!this->queue){
        return SUCCESS;
    }
    
    struct efpm_event_queue_s *tmp = this->queue;
    struct efpm_event_queue_s *prev = tmp;
    while(tmp){
        if(tmp->ev->fd == ev->fd){
            // queue의 시작인 경우
            if(this->queue == tmp) {
                this->queue = tmp->next;
            } else {
                // 중간이나 마지막인 경우
                prev->next = tmp->next;
            }
            free(tmp);
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }

    return SUCCESS;
}

int efpm_event_wait(struct efpm_event_module_s *this) {
    int timeout = -1;
    bool be_shutdown = false;
    while(1) {
        if(be_shutdown){
            return SUCCESS;
        }

        memset(this->epollfds, 0, sizeof(struct epoll_event) * this->nepollfds);
        int ret = epoll_wait(this->epollfd, this->epollfds, this->nepollfds, timeout);
        if(ret == -1){
            if(errno != EINTR){
                printf("epoll_wait(): %d\n", errno);
                return FAILURE;
            }
        }

        for(int i=0; i<ret; i++){
            if(!this->epollfds[i].data.ptr){
                continue;
            }



            struct efpm_event_s *ev = (struct efpm_event_s *)this->epollfds[i].data.ptr;
            if(ev->fd == this->efd){
                uint64_t cnt;
                while(1){
                    int n = read(this->efd, &cnt, sizeof(cnt));
                    if(n <= 0) {
                        break;
                    }
                    if(cnt == DO_SHUTDOWN) {
                        be_shutdown = true;
                        break;
                    }
                }
            }
            
            uint32_t flags = this->epollfds[i].events;
            printf("flags: %d\n", flags);
            (*this->fire)(this, flags, ev);
        }
    }
}

void efpm_event_fire(struct efpm_event_module_s *this,  uint32_t flags, struct efpm_event_s *ev) {
    if(!ev || !ev->callback) {
        return;
    }

    (*ev->callback)(ev, flags, ev->arg);
}

