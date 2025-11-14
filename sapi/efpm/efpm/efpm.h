#ifndef EFPM_H
#define EFPM_H 1

#include <unistd.h>
#include <sys/types.h>

#define EFPM_EXIT_OK 0
#define EFPM_EXIT_USAGE 64
#define EFPM_EXIT_SOFTWARE 70

#define EFPM_EXIT_CONFIG 78

enum efpm_init_return_status {
	EFPM_INIT_ERROR,
	EFPM_INIT_CONTINUE,
	EFPM_INIT_EXIT_OK,
};

int efpm_run(int *max_requests);
enum efpm_init_return_status efpm_init(int argc, char **argv);

struct efpm_globals_s {
	pid_t parent_pid;
	int argc;
	char **argv;
	char *config;
	char *prefix;
	char *pid;
	int running_children;
	int error_log_fd;
	int log_level;
	int listening_socket; /* for this child */
	int max_requests; /* for this child */
	int is_child;
	int test_successful;
	int heartbeat;
	int run_as_root;
	int force_stderr;
	int send_config_pipe[2];
};

extern struct efpm_globals_s efpm_globals;

#endif
