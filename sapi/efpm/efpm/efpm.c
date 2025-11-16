#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <errno.h>
#include "zend.h"

#include "efpm_event.h"
#include "efpm_worker.h"
#include "efpm.h"
#include "php_main.h"

int child_num = 0;

struct efpm_s *new_efpm(int workers, int reuseport) {
    struct efpm_s *efpm = (struct efpm_s *)malloc(sizeof(struct efpm_s));
    if(!efpm) {
        return NULL;
    }

    efpm->event_module = new_event_module(EVENT_SIZE);
    if(!efpm->event_module) {
        return NULL;
    }

    efpm->childs = (struct efpm_child_s **)malloc(sizeof(struct efpm_child_s*) * workers);
    if(!efpm->childs) {
        return NULL;
    }

    for(int i=0; i<workers; i++){
        efpm->childs[i] = new_efpm_child(i);
        if(!efpm->childs[i]) {
            return NULL;
        }
    }

    efpm->port = 9000;
    efpm->worker = workers;
    efpm->reuseport = reuseport;
    efpm->init = efpm_init;
    efpm->run = efpm_run;
    efpm->clean = efpm_clean;
    efpm->get_child = efpm_get_child;
}

void del_efpm(struct efpm_s *efpm) {
    if(!efpm){
        return;
    }

    (*efpm->clean)(efpm);
    free(efpm);
}

// one thread(accept) - multi thread(client)
int efpm_init(struct efpm_s *this) {
    int sock = efpm_socket(this->port);
    if(sock == FAILURE){
        return FAILURE;
    }

    this->listening_socket = sock;
    (*this->event_module->init)(this->event_module);

    int tfd = new_timerfd(1, 0);
    if(tfd == FAILURE){
        return FAILURE;
    }

    struct efpm_event_s *ev = efpm_event_set(tfd, &is_dead_child, this);
    (*this->event_module->add)(this->event_module, ev);

    return SUCCESS;
}

void is_dead_child(struct efpm_event_s *ev, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;

    uint64_t expirations;
    read(ev->fd, &expirations, sizeof(expirations)); 
    printf("hello i am parent: %d-%ld\n", this->worker, expirations);
}

int efpm_run(struct efpm_s *this) {
    return (*this->event_module->wait)(this->event_module);
//     for(int i=0; i<this->worker; i++){
//         pid_t pid = fork();
//         if(pid == 0) {
//             child_num = i;
//             goto child;

//         } else if(pid < 0){
//             // error
//         } else {
//             // parent
//         }
//     }

// parent:
//     // wait
//     return SUCCESS;

// child:
//     struct efpm_child_s *child = (*this->get_child)(this, child_num);
//     return (*child->run)(child);
}

struct efpm_child_s *efpm_get_child(struct efpm_s *this, int cn) {
    for(int i=0; i<this->worker; i++){
        if(i==cn){
            return this->childs[i];
        }
    }

    return NULL;
}

int efpm_clean(struct efpm_s *this){
    if(!this->childs){
        return SUCCESS;
    }

    for(int i=0;i<this->worker;i++){
        struct efpm_child_s *child = this->childs[i];
        if(!child){
            continue;
        }

        (*child->clean)(child);
        free(this->childs[i]);
    }

    // 이벤트 모듈 정리
    (*this->event_module->clean)(this->event_module);

    free(this->childs);
    return SUCCESS;
}

int efpm_socket(int port) {
    int flags = 1;
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    server_addr.sin_port = htons(9000);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        printf("socket: %d\n", errno);
        return FAILURE;
    }

    // if(setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &flags, sizeof(flags)) < 0){
    //     printf("setsockopt: %d\n", errno);
    //     return FAILURE;
    // }

    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags)) < 0) {
        printf("setsockopt: %d\n", errno);
        return FAILURE;
    }

    if(bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("bind: %d\n", errno);
        return FAILURE;
    }

    if(listen(sock, 512) < 0) {
        printf("listen: %d\n", errno);
        return FAILURE;
    }

    return sock;
}

/*
int efpm_network_init(int workers, int port, bool reuseport) {
    efpm_globals.listening_socket = malloc(sizeof(int) * workers);
    if(efpm_globals.listening_socket == NULL) {
        return FAILURE;
    }

    int sock;
    bool oneshot = false;
    for(int i=0;i<workers;i++) {
        if(!oneshot){
            sock = efpm_socket(port, reuseport);
            if(sock == FAILURE){
                return FAILURE;
            }
        }

        efpm_globals.listening_socket[i] = sock;
        if(!reuseport){
            oneshot = true;
        }
    }

    return SUCCESS;
}

void efpm_network_shutdown() {
    free(efpm_globals.listening_socket);
}

int efpm_worker_new(int n) {
    int p2c[2];
    pipe(p2c);
    pid_t pid = fork(); 
    if(pid == 0) {
        // child
        php_child_init();
        efpm_globals.is_child = 1;
        efpm_globals.child_num = n;
        efpm_globals.w_fd = p2c[WRITE_SP];
        if(prctl(PR_SET_NAME, "my-child-process") < 0){
            printf("failed to prctl\n");
        }
        close(p2c[READ_SP]);
        return 0;
    } else if(pid < 0) {
        return -1;
    }
    
    // parent 
    close(p2c[WRITE_SP]);
    struct efpm_event_s *ev = malloc(sizeof(struct efpm_event_s));
    efpm_event_set(ev, p2c[READ_SP], &worker_callback, NULL);
    efpm_event_module.add(ev);

    return 1;
}

enum efpm_init_return_status efpm_init(int workers) { 
    if(efpm_network_init(workers, 9000, true) == FAILURE) {
        return EFPM_INIT_ERROR;
    }

    if(efpm_event_module.init(512) == FAILURE) {
        return EFPM_INIT_ERROR;
    }

    return EFPM_INIT_CONTINUE;
}

int efpm_run(int workers) {
    for(int i=0;i<workers;i++){
        int is_parent = efpm_worker_new(i);
        if(is_parent == -1){
            printf("failed to fork\n");
            return FAILURE;
        }

        if(!is_parent){
            return SUCCESS;
        }

        //p2c[READ_SP] event 등록 - child process는 signal 탐지시 WRITE_SP로 전송
        // 부모는 수집후 waitpid로 자식 제거 및 대기
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) { perror("timerfd_create"); exit(1); }

    // 1초 후에 첫 발사, 그 이후 1초마다 반복
    struct itimerspec its;
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 1;     // 주기
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        exit(1);
    }
    
    struct efpm_event_s *ev = malloc(sizeof(struct efpm_event_s));
    efpm_event_set(ev, tfd, &worker_sig_handle, NULL);
    efpm_event_module.add(ev);

    return efpm_event_module.wait();
}  
*/