#ifndef EFPM_WORKER_H
#define EFPM_WORKER_H

#include "efpm_event.h"

void efpm_child_request_accepting();
void efpm_child_request_reading_headers();
void efpm_child_request_finished();

#endif