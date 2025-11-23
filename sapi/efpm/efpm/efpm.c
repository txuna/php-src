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

#include "SAPI.h"
#include "fastcgi.h"

#include "zend.h"

#include "efpm_event.h"
#include "efpm.h"
#include "php_main.h"

extern int efpm_request_body_fd;
extern int efpm_is_running;
extern struct efpm_s *efpm_g;

static fcgi_request *efpm_init_request(int listen_fd);

struct efpm_s *new_efpm() {
    struct efpm_s *efpm = (struct efpm_s *)malloc(sizeof(struct efpm_s));
    if(!efpm) {
        return NULL;
    }

    efpm->event_module = new_event_module(EVENT_SIZE);
    if(!efpm->event_module) {
        return NULL;
    }

    signal(SIGPIPE, SIG_IGN);

    efpm->port = 9000;
    efpm->init = efpm_init;
    efpm->run = efpm_run;
    efpm->clean = efpm_clean;

    // 전역변수 세팅
    efpm_g = efpm;

    return efpm;    
}

void del_efpm(struct efpm_s *efpm) {
    if(!efpm){
        return;
    }

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

    struct efpm_event_s *ev = efpm_event_set(efd, &efpm_server_event, this);
    (*this->event_module->add)(this->event_module, ev);

    // listening socket 등록
    struct efpm_event_s *ev2 = efpm_event_set(this->listening_socket, &efpm_accept_client, this); 
    (*this->event_module->add)(this->event_module, ev2);

    return SUCCESS;
}

void catch_signal(struct efpm_event_s *ev, uint32_t flags, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;

    struct signalfd_siginfo si;
    uint64_t v = DO_SHUTDOWN;
    read(ev->fd, &si, sizeof(si));
    if(si.ssi_code == SIGPIPE) {
        printf("sig pipe, do ignore\n");
        return;
    }

    write(this->efd, &v, sizeof(v));    
}

void efpm_server_event(struct efpm_event_s *ev, uint32_t flags, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;
    printf("serve event!\n");
}

#define SIG_BLOCK 0

int efpm_run(struct efpm_s *this) {
    efpm_is_running = 1;

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

    struct efpm_event_s *ev = efpm_event_set(sfd, &catch_signal, this);
    (*this->event_module->add)(this->event_module, ev);

    // wait
    int ret = (*this->event_module->wait)(this->event_module);
    if(ret == FAILURE){
        return FAILURE;
    }

    return (*this->clean)(this);
}

int efpm_clean(struct efpm_s *this){

    // 이벤트 모듈 정리
    (*this->event_module->clean)(this->event_module);

    free(this->event_module);
    close(this->listening_socket);

    fcgi_shutdown();
    php_module_shutdown();
    sapi_shutdown();

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

/*
    new API for fastcgi
    1. fcgi_get_fd(fcgi_request *request);
    2. fcgi_get_listenfd(fcgi_request* request);
    3. fcgi_accept_request2(fcgi_request* request);
    4. int fcgi_set_fd(fcgi_request *req, int fd);
    5. int fcgi_process_request(fcgi_request *req);
    6. int get_peer_addr(int fd, struct sockaddr_in *addr);
    7. const char *fcgi_get_client_ip(struct sockaddr_in sa);
*/
void efpm_accept_client(struct efpm_event_s *ev, uint32_t flags, void *arg) {
    struct efpm_s *this = (struct efpm_s *)arg;

    struct sockaddr_in sa; 
    unsigned int len = sizeof(sa); 

    fcgi_request *request = efpm_init_request(this->listening_socket);
    int client_fd = fcgi_accept_request2(request);
    if(client_fd == -1) {
        printf("failed to fcgi_accept_request2()\n");
        return;
    }

    struct efpm_event_s *ev2 = efpm_event_set(client_fd, &efpm_handle_client, request);
    (*this->event_module->add)(this->event_module, ev2);
}

static fcgi_request *efpm_init_request(int listen_fd) {
    fcgi_request *req = fcgi_init_request(listen_fd,
        efpm_request_accepting,   /* on_accept */
        efpm_request_reading_headers,   /* on_read */
        efpm_request_finished); /* on_close */

    return req;
}

void efpm_request_accepting(void) {
    return; 
}

void efpm_request_reading_headers(void) {
    return;
}

void efpm_request_finished(void) {
    return;
}

// void efpm_handle_client(struct efpm_event_s *ev, void *arg) {

// }