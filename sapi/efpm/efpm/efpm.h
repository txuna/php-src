#ifndef EFPM_H
#define EFPM_H 1

#include <unistd.h>
#include <sys/types.h>

#define EFPM_EXIT_OK 0
#define EFPM_EXIT_USAGE 64
#define EFPM_EXIT_SOFTWARE 70

#define EFPM_EXIT_CONFIG 78

#define DO_SHUTDOWN 1

enum efpm_init_return_status {
	EFPM_INIT_ERROR,
	EFPM_INIT_CONTINUE,
	EFPM_INIT_EXIT_OK,
};

struct efpm_conn_s {

};

struct efpm_s {
	int worker;
	int reuseport; 
	int listening_socket;
	int port;
	int efd;
	bool shutdown_sig;

	struct efpm_event_module_s *event_module;
	int (*init)(struct efpm_s *this);
	int (*run)(struct efpm_s *this);
	int (*clean)(struct efpm_s *this);
};

// parent
int efpm_init(struct efpm_s *this);
int efpm_run(struct efpm_s *this);
int efpm_clean(struct efpm_s *this);
void efpm_server_event(struct efpm_event_s *ev, uint32_t flags, void *arg);
void catch_signal(struct efpm_event_s *ev, uint32_t flags, void *arg);
void efpm_accept_client(struct efpm_event_s *ev, uint32_t flags, void *arg);
void efpm_handle_client(struct efpm_event_s *ev, uint32_t flags, void *arg); 

void efpm_request_accepting(void);
void efpm_request_reading_headers(void);
void efpm_request_finished(void);

struct efpm_s *new_efpm();
void del_efpm(struct efpm_s *efpm);
int efpm_socket(int port);

#endif
