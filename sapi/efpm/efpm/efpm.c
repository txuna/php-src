#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>

#include "zend.h"

#include "efpm_event.h"
#include "efpm_worker.h"
#include "efpm.h"
#include "php_main.h"


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

    // eventfd 
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(efd == -1){
        return FAILURE;
    }

    this->event_module->efd = efd;
    this->efd = efd;

    struct efpm_event_s *ev = efpm_event_set(efd, &efpm_signal_dead, this);
    (*this->event_module->add)(this->event_module, ev);

    return SUCCESS;
}

void catch_signal(struct efpm_event_s *ev, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;
    
    struct signalfd_siginfo si;
    uint64_t v = DO_SHUTDOWN;
    ssize_t n = read(ev->fd, &si, sizeof(si));
    printf("catch signal: %d from: %d\n", si.ssi_signo, si.ssi_pid);

    for(int i=0; i<this->worker; i++){
        struct efpm_child_s *child = this->childs[i];
        if(si.ssi_pid == child->pid){
            v = DO_CHILD;
        }
    }

    write(this->efd, &v, sizeof(v));
}

void efpm_signal_dead(struct efpm_event_s *ev, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;

    int status = 0;
    while(1){
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
        if(pid < 0){
            return;
        }

        if(pid == 0){
            return;
        }

        // 재부활 로직 추가 필요
        if (WIFEXITED(status)) {
            printf("child %d exited, status=%d\n", pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("child %d killed by signal %d\n", pid, WTERMSIG(status));
        }
    }
}

int efpm_run(struct efpm_s *this) {
    struct efpm_child_s *child;
    for(int i=0; i<this->worker; i++){
        child = this->childs[i];
        pid_t pid = fork();
        if(pid == 0) {
            goto child;
        } else if(pid < 0){
            // error
        } else {
            // parent
            child->pid = pid;
        }
    }

    // 시그널 감지 추가
    sigset_t mask;
    int sfd; 
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);   // Ctrl+C
    sigaddset(&mask, SIGTERM);  // kill -TERM
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGPIPE);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        printf("sigprocmask(): %d\n", errno);
        return FAILURE;
    }

    sfd = signalfd(-1, &mask, SFD_CLOEXEC); 
    if(sfd == -1){
        printf("signalfd(): %d\n", errno);
        return FAILURE;
    }

    struct efpm_event_s *ev2 = efpm_event_set(sfd, &catch_signal, this);
    (*this->event_module->add)(this->event_module, ev2);

    // wait
    int ret = (*this->event_module->wait)(this->event_module);
    if(ret == FAILURE){
        return FAILURE;
    }

    return (*this->clean)(this);

child:
    return (*child->run)(child);
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

    // 자식 프로세스 정리
    for(int i=0;i<this->worker;i++){
        struct efpm_child_s *child = this->childs[i];
        if(!child){
            continue;
        }

        kill(child->pid, SIGTERM);
        free(this->childs[i]);
    }

    // 이벤트 모듈 정리
    (*this->event_module->clean)(this->event_module);

    free(this->event_module);
    free(this->childs);
    close(this->listening_socket);

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
