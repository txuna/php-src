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

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <signal.h>

#include <locale.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

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
#include "efpm.h"

/*
sudo apt install make autoconf gcc cmake pkg-config flex bison re2c libxml2-dev sqlite3 libsqlite3-dev -y

./buildconf —force
./configure --enable-debug --disable-cgi --enable-efpm --disable-fpm --disable-phpdbg
./config.nice
make -j $(nproc)
sudo make install

php-efpm
*/

static int php_cgi_startup(struct _sapi_module_struct *sapi_module) {
    return php_module_startup(sapi_module, NULL);
}
/* }}} */

/* {{{ sapi_module_struct cgi_sapi_module */
static sapi_module_struct cgi_sapi_module = {
    .name = "efpm-fcgi",    /* name */
    .pretty_name = "EFPM/FastCGI", /* pretty name */

    /* minit startup */
    .startup = php_cgi_startup,
    /* mshutdown shutdown */
    .shutdown = php_module_shutdown_wrapper,
    STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

#ifndef HAVE_ATTRIBUTE_WEAK
static void fcgi_log(int type, const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}
#endif

/*
    sapi_startup()

    MINIT   

    RINIT
    RSHUTDOWN 

    RINIT 
    RSHUTDOWN

    MSHUTDOWN

    sapi_shutdown()
*/

static const char HARDCODED_INI[] =
"html_errors=0\nregister_argc_argv=1\nimplicit_flush=1\noutput_buffering=0\n"
"max_execution_time=0\n";

// 없으면 dummy
static fcgi_request *efpm_init_request(int listen_fd) {
    fcgi_request *req = fcgi_init_request(listen_fd,
        NULL,   /* on_accept */
        NULL,   /* on_read */
        NULL); /* on_close */

    return req;
}

/* {{{ main */
int main(int argc, char **argv) {
    PHPWRITE("Hello\n", 6);

    sapi_startup(&cgi_sapi_module); // php_module_startup 포함됨.

#ifndef HAVE_ATTRIBUTE_WEAK
	fcgi_set_logger(fcgi_log);
#endif

    fcgi_init();

    cgi_sapi_module.ini_entries = malloc(sizeof(HARDCODED_INI));
    memcpy((void*)cgi_sapi_module.ini_entries, HARDCODED_INI, sizeof(HARDCODED_INI));

    cgi_sapi_module.additional_functions = NULL;
    cgi_sapi_module.executable_location = argv[0];
    if(cgi_sapi_module.startup(&cgi_sapi_module) == FAILURE) {
        return EFPM_EXIT_SOFTWARE;
    }

    if(efpm_init(argc, argv) == EFPM_INIT_ERROR){
        return EFPM_EXIT_SOFTWARE;
    }

    int fcgi_fd = efpm_globals.listening_socket;
    fcgi_request *request = efpm_init_request(fcgi_fd);

    for(;;){
        if(fcgi_accept_request(request) < 0){
            break;
        }
    }

    // request loop
    php_request_startup();

    php_request_shutdown((void *)0);

    //
    fcgi_shutdown();
    php_module_shutdown();
    sapi_shutdown();

    return 0;
}
/* }}} */