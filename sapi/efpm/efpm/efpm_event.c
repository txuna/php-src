#include <stdio.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/prctl.h>

#include "zend.h"
#include "efpm.h"
#include "efpm_event.h"

static struct epoll_event *epollfds = NULL; 
static int nepollfds = 0;
static int epollfd = -1;

struct efpm_event_module_s efpm_event_module = {
    .name = "php-efpm event module",
    .init = efpm_event_init_main,
    .clean = efpm_event_clean,
    .wait = efpm_event_wait, 
    .add = efpm_event_add, 
    .remove = efpm_event_remove,
};

int efpm_event_init_main(int max) {
    if(max <= 0) {
        return FAILURE;
    }

    epollfd = epoll_create(max);
    if(epollfd < 0){
        printf("failed to epoll_create(): %d\n", epollfd);
        return FAILURE;
    }

    epollfds = malloc(sizeof(struct epoll_event) * max);
    if(!epollfds){
        printf("failed to allocate epollfds\n");
        return FAILURE;
    }

    memset(epollfds, 0, sizeof(struct epoll_event) * max);

    nepollfds = max;

    return SUCCESS;
}

int efpm_event_clean() {
    if(epollfds) {
        free(epollfds);
        epollfds = NULL;
    }

    if(epollfd != -1){
        close(epollfd);
        epollfd = -1;
    }

    nepollfds = 0;
    return SUCCESS;
}

int efpm_event_wait() {
    int timeout = -1; // milliseconds
    while(1){
        memset(epollfds, 0, sizeof(struct epoll_event) * nepollfds);
        int ret = epoll_wait(epollfd, epollfds, nepollfds, timeout);
        if(ret == -1){
            if(errno != EINTR){
                printf("epoll_wait(): %d\n", errno);
                return FAILURE;
            }
        }

        for(int i=0;i<ret;i++) {
            if(!epollfds[i].data.ptr){
                continue;
            }

            if(efpm_globals.is_child){
                return 0;
            }

            struct efpm_event_s *ev = (struct efpm_event_s *)epollfds[i].data.ptr;
            efpm_event_fire(ev);
        }
    }

    return 0;
}

int efpm_event_set(struct efpm_event_s *ev, int fd, void (*callback)(struct efpm_event_s *), void *arg) {
	if (!ev || !callback || fd < -1) {
		return -1;
	}
	memset(ev, 0, sizeof(struct efpm_event_s));
	ev->fd = fd;
	ev->callback = callback;
	ev->arg = arg;

    printf("set: ev->fd: %d\n", ev->fd);
	return 0;
}

int efpm_event_add(struct efpm_event_s *ev) {
    struct epoll_event e;

    e.events = EPOLLIN;
    e.data.ptr = ev;
    // e.data.fd = ev->fd;

    // level trigger 
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, ev->fd, &e) == -1) {
        printf("failed to epoll_ctl(): fd: %d\n", ev->fd);
        return FAILURE;
    }

    ev->index = ev->fd;
    return SUCCESS;
}

// queue에서 지워야할듯
int efpm_event_remove(struct efpm_event_s *ev) {
    struct epoll_event e;
    
    e.events = EPOLLIN;
    e.data.fd = ev->fd;
    e.data.ptr = (void*)ev;

    if(epoll_ctl(epollfd, EPOLL_CTL_DEL, ev->fd, &e) == -1){
        printf("failed to epoll_ctl: fd: %d\n", ev->fd);
        return FAILURE;
    }

    ev->index = -1;
    return SUCCESS;
}

void efpm_event_fire(struct efpm_event_s *ev) {
    if(!ev || !ev->callback) {
        return;
    }

    (*ev->callback)(ev);
}
