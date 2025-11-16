#ifndef EFPM_H
#define EFPM_H 1

#include <unistd.h>
#include <sys/types.h>

#define EFPM_EXIT_OK 0
#define EFPM_EXIT_USAGE 64
#define EFPM_EXIT_SOFTWARE 70

#define EFPM_EXIT_CONFIG 78

#define DO_SHUTDOWN 1
#define DO_CHILD 2

enum efpm_init_return_status {
	EFPM_INIT_ERROR,
	EFPM_INIT_CONTINUE,
	EFPM_INIT_EXIT_OK,
};

struct efpm_conn_s {

};

struct efpm_child_s {
	pid_t pid;
	pid_t parent_pid;
	int child_num;
	struct efpm_event_module_s *event_module;

	int (*init)(struct efpm_child_s *this);
	int (*run)(struct efpm_child_s *this);
	int (*clean)(struct efpm_child_s *this);
};

struct efpm_s {
	int worker;
	int reuseport; 
	int listening_socket;
	int port;
	int efd;

	struct efpm_event_module_s *event_module;
	struct efpm_child_s **childs;
	int (*init)(struct efpm_s *this);
	int (*run)(struct efpm_s *this);
	int (*clean)(struct efpm_s *this);
	struct efpm_child_s* (*get_child)(struct efpm_s *this, int cn);
};

// parent
int efpm_init(struct efpm_s *this);
int efpm_run(struct efpm_s *this);
int efpm_clean(struct efpm_s *this);
struct efpm_child_s *efpm_get_child(struct efpm_s *this, int cn);
void efpm_signal_dead(struct efpm_event_s *ev, void *arg);
void catch_signal(struct efpm_event_s *ev, void *arg);

// child
int efpm_child_init(struct efpm_child_s *this);
int efpm_child_run(struct efpm_child_s *this);
int efpm_child_clean(struct efpm_child_s *this);

struct efpm_child_s *new_efpm_child(int cn);
struct efpm_s *new_efpm(int workers, int reuseport);
void del_efpm(struct efpm_s *efpm);
int efpm_socket(int port);

struct efpm_globals_s {
	pid_t parent_pid;
	int *listening_socket; /* for this child */
	int is_child;
	int child_num;
	int w_fd; 
};

extern struct efpm_globals_s efpm_globals;

#endif
