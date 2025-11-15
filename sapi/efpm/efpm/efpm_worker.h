#ifndef EFPM_WORKER_H
#define EFPM_WORKER_H

#include "efpm_event.h"

void worker_sig_handle(struct efpm_event_s *ev);
void worker_callback(struct efpm_event_s *ev);

#endif