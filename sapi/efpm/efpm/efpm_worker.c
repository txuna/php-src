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

int efpm_child_init(struct efpm_child_s *this){
    return SUCCESS;
}

int efpm_child_run(struct efpm_child_s *this){
    return SUCCESS;
}

int efpm_child_clean(struct efpm_child_s *this){
    return SUCCESS;
}

// void worker_sig_handle(struct efpm_event_s *ev) {
//     int status = 0;
//     while(1){
//         pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
//         if(pid < 0) {
//             printf("waitpid(): %d\n", errno);
//             return;
//         }

//         if(pid == 0){
//             return;
//         }

//         if (WIFEXITED(status)) {
//             printf("child %d exited, status=%d\n", pid, WEXITSTATUS(status));
//         } else if (WIFSIGNALED(status)) {
//             printf("child %d killed by signal %d\n", pid, WTERMSIG(status));
//         }
//    }
// }
