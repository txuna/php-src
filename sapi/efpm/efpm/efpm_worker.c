#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include "efpm_event.h"
#include "zend.h"
#include "efpm.h"

struct efpm_child_s *new_efpm_child(int cn) {
    struct efpm_child_s *child = (struct efpm_child_s *)malloc(sizeof(struct efpm_child_s));
    if(!child){
        return NULL;
    }

    child->event_module = new_event_module(EVENT_SIZE);
    if(!child->event_module){
        free(child);
        return NULL;
    }

    child->child_num = cn;
    child->clean = efpm_child_clean;
    child->init = efpm_child_init;
    child->run = efpm_child_run;

    return child;
}

int efpm_child_init(struct efpm_child_s *this){
    return SUCCESS;
}

int efpm_child_run(struct efpm_child_s *this){
    while(1){
        sleep(1);
    }
    return SUCCESS;
}

int efpm_child_clean(struct efpm_child_s *this){
    (*this->event_module->clean)(this->event_module);
    free(this->event_module);
    return SUCCESS;
}
