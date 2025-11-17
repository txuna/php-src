#ifndef EFPM_WORKER_H
#define EFPM_WORKER_H

#include "efpm_event.h"

void efpm_child_request_accepting(void);
void efpm_child_request_reading_headers(void);
void efpm_child_request_finished(void);
void efpm_child_new_connection(struct efpm_event_s *ev, void *arg);
void efpm_child_handle_connection(struct efpm_event_s *ev, void *arg);

#endif