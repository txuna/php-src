#ifndef EFPM_EVENT_H
#define EFPM_EVENT_H 1

#define EVENT_SIZE 1024

struct efpm_event_s {
    int fd; 
    int index;
    void (*callback)(struct efpm_event_s *, void *);
    void *arg;
};

struct efpm_event_queue_s {
    struct efpm_event_queue_s *next;
    struct efpm_event_s *ev;
};

struct efpm_event_module_s {
    const char *name;
    int efd; 
    int epollfd;
    int nepollfds; 
    struct epoll_event *epollfds;
    struct efpm_event_queue_s *queue;

    int (*init)(struct efpm_event_module_s *this);
    int (*clean)(struct efpm_event_module_s *this);
    int (*wait)(struct efpm_event_module_s *this);
    int (*add)(struct efpm_event_module_s *this, struct efpm_event_s *ev);
    int (*remove)(struct efpm_event_module_s *this, struct efpm_event_s *ev);
    void (*fire)(struct efpm_event_module_s *this, struct efpm_event_s *ev);
};

struct efpm_event_module_s *new_event_module(int max);
struct efpm_event_s *efpm_event_set(int fd, void (*callback)(struct efpm_event_s *, void *arg), void *arg);
int new_timerfd(int sec, int milli);

void efpm_event_fire(struct efpm_event_module_s *this, struct efpm_event_s *ev) ;
int efpm_event_init_main(struct efpm_event_module_s *this);
int efpm_event_clean(struct efpm_event_module_s *this);
int efpm_event_wait(struct efpm_event_module_s *this);
int efpm_event_add(struct efpm_event_module_s *this, struct efpm_event_s *ev);
int efpm_event_remove(struct efpm_event_module_s *this, struct efpm_event_s *ev);

extern struct efpm_event_module_s efpm_event_module;

#endif