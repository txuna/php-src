#include "php.h"
#include "php_globals.h"
#include "php_variables.h"
#include "php_ini_builder.h"
#include "zend_modules.h"
#include "php.h"
#include "zend_ini_scanner.h"
#include "zend_globals.h"
#include "zend_stream.h"

#include "SAPI.h"

#include <stdio.h>
#include "php.h"

#include <signal.h>
#include <locale.h>

#include "zend.h"
#include "zend_extensions.h"
#include "php_ini.h"
#include "php_globals.h"
#include "php_main.h"
#include "fopen_wrappers.h"
#include "ext/standard/php_standard.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_highlight.h"
#include "php_getopt.h"
#include "http_status_codes.h"
#include "fastcgi.h"
#include <php_config.h>
#include "efpm_event.h"
#include "efpm.h"
#include "efpm_config.h"
#include "efpm_worker.h"

struct efpm_child_s *child_g = NULL;

struct efpm_child_s *new_efpm_child(int child_num, int sock) {
    struct efpm_child_s *child = (struct efpm_child_s *)malloc(sizeof(struct efpm_child_s));
    if(!child){
        return NULL;
    }

    child->event_module = new_event_module(EVENT_SIZE);
    if(!child->event_module){
        free(child);
        return NULL;
    }

    child->child_num = child_num;
    child->fcgi_fd = sock;
    child->clean = efpm_child_clean;
    child->init = efpm_child_init;
    child->run = efpm_child_run;

    return child;
}

static fcgi_request *efpm_init_request(int listen_fd) {
    fcgi_request *req = fcgi_init_request(listen_fd,
        efpm_child_request_accepting,   /* on_accept */
        efpm_child_request_reading_headers,   /* on_read */
        efpm_child_request_finished); /* on_close */

    return req;
}

void efpm_child_request_accepting() {
    struct efpm_child_s *this = child_g;
}

void efpm_child_request_reading_headers() {
    struct efpm_child_s *this = child_g;
}

void efpm_child_request_finished() {
    struct efpm_child_s *this = child_g;
}

int efpm_child_init(struct efpm_child_s *this){
    this->parent_pid = getppid();
    this->pid = getpid();

    php_child_init();
    (*this->event_module->init)(this->event_module);

    return SUCCESS;
}

/*
    new API for fastcgi
    1. fcgi_get_fd(fcgi_request *request);
    2. fcgi_get_listenfd(fcgi_request* request);
    3. fcgi_accept_request2(fcgi_request* request);
*/
int efpm_child_run(struct efpm_child_s *this){
    // fcgi_request *request = efpm_init_request(this->fcgi_fd);
    // struct efpm_event_s *ev = efpm_event_set(fcgi_get_listenfd(request), &efpm_child_fire, request);
    
    struct efpm_event_s *ev = efpm_event_set(this->fcgi_fd, &efpm_child_new_connection, this);
    (*this->event_module->add)(this->event_module, ev);

    return (*this->event_module->wait)(this->event_module);
}

void efpm_child_new_connection(struct efpm_event_s *ev, void *arg) {
    struct sockaddr_in sa;
    unsigned int len = sizeof(sa);
    struct efpm_child_s *this = (struct efpm_child_s *)arg;

    int client_fd = accept(this->fcgi_fd, (struct sockaddr *)&sa, &len);
    
    printf("new client: %d\n", client_fd);
    fcgi_request *request = efpm_init_request(this->fcgi_fd);

    struct efpm_event_s *ev2 = efpm_event_set(client_fd, &efpm_child_handle_connection, this);
    (*this->event_module->add)(this->event_module, ev2);
}

void efpm_child_handle_connection(struct efpm_event_s *ev, void *arg) {
    struct efpm_child_s *this = child_g;
    fcgi_request *request = (fcgi_request *)arg;

    // printf("HANDLE CLIENT: %d\n", fcgi_get_fd(request));
    
}

int efpm_child_clean(struct efpm_child_s *this){
    (*this->event_module->clean)(this->event_module);
    free(this->event_module);
    return SUCCESS;
}
