#ifndef EFPM_EVENT_H
#define EFPM_EVENT_H 1

struct efpm_event_s {
    int fd; 
    int index;
    void (*callback)(struct efpm_event_s *);
    void *arg;
};

struct efpm_event_module_s {
    const char *name;
    int (*init)(int max);
    int (*clean)(void);
    int (*wait)();
    int (*add)(struct efpm_event_s *ev);
    int (*remove)(struct efpm_event_s *ev);
};

int efpm_event_set(struct efpm_event_s *ev, int fd, void (*callback)(struct efpm_event_s *), void *arg);
void efpm_event_fire(struct efpm_event_s *ev);
int efpm_event_init_main(int max);
int efpm_event_clean();
int efpm_event_wait();
int efpm_event_add(struct efpm_event_s *ev);
int efpm_event_remove(struct efpm_event_s *ev);

extern struct efpm_event_module_s efpm_event_module;

#endif