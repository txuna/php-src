#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "efpm.h"

struct efpm_globals_s efpm_globals = {
	.parent_pid = 0,
	.argc = 0,
	.argv = NULL,
	.config = NULL,
	.prefix = NULL,
	.pid = NULL,
	.running_children = 0,
	.error_log_fd = 0,
	.log_level = 0,
	.listening_socket = 0,
	.max_requests = 0,
	.is_child = 0,
	.test_successful = 0,
	.heartbeat = 0,
	.run_as_root = 0,
	.force_stderr = 0,
	.send_config_pipe = {0, 0},
};

enum efpm_init_return_status efpm_init(int argc, char **argv)
{   
    int flags = 1;
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    server_addr.sin_port = htons(9000);


    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 ){
        return EFPM_INIT_ERROR;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags)) < 0) {
        return EFPM_INIT_ERROR;
    }

    if(bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        return EFPM_INIT_ERROR;
    }

    if(listen(sock, 5) < 0) {
        return EFPM_INIT_ERROR;
    }

    efpm_globals.listening_socket = sock;
    return EFPM_INIT_CONTINUE;
}
