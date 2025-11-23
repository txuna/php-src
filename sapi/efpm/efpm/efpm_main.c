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
#include "efpm_event.h"
#include "efpm.h"
#include "efpm_config.h"

/*
sudo apt install make autoconf gcc cmake pkg-config flex bison re2c libxml2-dev sqlite3 libsqlite3-dev -y

./buildconf —force
./configure --enable-debug --disable-cgi --enable-efpm --disable-fpm --disable-phpdbg
./config.nice
make -j $(nproc)
sudo make install

php-efpm
*/

static int sapi_cgi_active();
static int sapi_cgi_deactivate();
static char *sapi_cgibin_getenv(const char *name, size_t name_len);
static void sapi_cgi_register_variables(zval *track_vars_array);
static size_t sapi_cgi_read_post(char *buffer, size_t count_bytes);
static char *sapi_cgi_read_cookies(void);
static int sapi_cgi_send_headers(sapi_headers_struct *sapi_headers);
static void sapi_cgibin_flush(void *server_context);
static inline size_t sapi_cgibin_single_write(const char *str, uint32_t str_length) ;
static size_t sapi_cgibin_ub_write(const char *str, size_t str_length);
static int php_cgi_startup(struct _sapi_module_struct *sapi_module);
void efpm_request_accepting();
void efpm_request_reading_headers();
void efpm_request_finished();
static int efpm_php_zend_ini_alter_master(char *name, int name_length, char *new_value, int new_value_length, int mode, int stage);
int efpm_php_apply_defines_ex(struct key_value_s *kv, int mode);
static int is_valid_path(const char *path);
static void fastcgi_ini_parser(zval *arg1, zval *arg2, zval *arg3, int callback_type, void *arg);
static void init_request_info(void);
// static fcgi_request *efpm_init_request(int listen_fd);

typedef struct _php_cgi_globals_struct {
	bool rfc2616_headers;
	bool nph;
	bool fix_pathinfo;
	bool discard_path;
	bool fcgi_script_path_encoded;
	bool fcgi_logging;
	bool fcgi_logging_request_started;
	HashTable user_config_cache;
	char *error_header;
	char *fpm_config;
} php_cgi_globals_struct;

static php_cgi_globals_struct php_cgi_globals;
#define CGIG(v) (php_cgi_globals.v)

int efpm_request_body_fd; // 개트롤 변수네
int efpm_is_running = 0;
struct efpm_s *efpm_g;

static int sapi_cgi_active() {
    return SUCCESS;
}

static int sapi_cgi_deactivate() {
	/* flush only when SAPI was started. The reasons are:
		1. SAPI Deactivate is called from two places: module init and request shutdown
		2. When the first call occurs and the request is not set up, flush fails on FastCGI.
	*/
	if (SG(sapi_started)) {
		if (!fcgi_finish_request((fcgi_request*)SG(server_context), 0)) {
			php_handle_aborted_connection();
		}
	}
	return SUCCESS;
}

static char *sapi_cgibin_getenv(const char *name, size_t name_len) /* {{{ */
{
	/* if fpm has started, use fcgi env */
	if (efpm_is_running) {
		fcgi_request *request = (fcgi_request*) SG(server_context);
		return fcgi_getenv(request, name, name_len);
	}

	/* if fpm has not started yet, use std env */
	return getenv(name);
}
/* }}} */

/* {{{ sapi_cgi_log_message */
static void sapi_cgi_log_message(const char *message, int syslog_type_int)
{
	// zlog_msg(ZLOG_NOTICE, "PHP message: ", message);
    printf("PHP message: %s\n", message);
}
/* }}} */

static void sapi_cgi_register_variables(zval *track_vars_array) /* {{{ */
{
	size_t php_self_len;
	char *php_self;

	/* In CGI mode, we consider the environment to be a part of the server
	 * variables
	 */
	php_import_environment_variables(track_vars_array);

	if (CGIG(fix_pathinfo)) {
		char *script_name = SG(request_info).request_uri;
		unsigned int script_name_len = script_name ? strlen(script_name) : 0;
		char *path_info = sapi_cgibin_getenv("PATH_INFO", sizeof("PATH_INFO") - 1);
		unsigned int path_info_len = path_info ? strlen(path_info) : 0;

		php_self_len = script_name_len + path_info_len;
		php_self = emalloc(php_self_len + 1);

		/* Concat script_name and path_info into php_self */
		if (script_name) {
			memcpy(php_self, script_name, script_name_len + 1);
		}
		if (path_info) {
			memcpy(php_self + script_name_len, path_info, path_info_len + 1);
		}

		/* Build the special-case PHP_SELF variable for the CGI version */
		if (sapi_module.input_filter(PARSE_SERVER, "PHP_SELF", &php_self, php_self_len, &php_self_len)) {
			php_register_variable_safe("PHP_SELF", php_self, php_self_len, track_vars_array);
		}
		efree(php_self);
	} else {
		php_self = SG(request_info).request_uri ? SG(request_info).request_uri : "";
		php_self_len = strlen(php_self);
		if (sapi_module.input_filter(PARSE_SERVER, "PHP_SELF", &php_self, php_self_len, &php_self_len)) {
			php_register_variable_safe("PHP_SELF", php_self, php_self_len, track_vars_array);
		}
	}
}
/* }}} */

static size_t sapi_cgi_read_post(char *buffer, size_t count_bytes) /* {{{ */
{
	uint32_t read_bytes = 0;
	int tmp_read_bytes;
	size_t remaining = SG(request_info).content_length - SG(read_post_bytes);

	if (remaining < count_bytes) {
		count_bytes = remaining;
	}
	while (read_bytes < count_bytes) {
		fcgi_request *request = (fcgi_request*) SG(server_context);
		if (efpm_request_body_fd == -1) {
			char *request_body_filename = FCGI_GETENV(request, "REQUEST_BODY_FILE");

			if (request_body_filename && *request_body_filename) {
				efpm_request_body_fd = open(request_body_filename, O_RDONLY);

				if (0 > efpm_request_body_fd) {
					php_error(E_WARNING, "REQUEST_BODY_FILE: open('%s') failed: %s (%d)",
							request_body_filename, strerror(errno), errno);
					return 0;
				}
			}
		}

		/* If REQUEST_BODY_FILE variable not available - read post body from fastcgi stream */
		if (efpm_request_body_fd < 0) {
			tmp_read_bytes = fcgi_read(request, buffer + read_bytes, count_bytes - read_bytes);
		} else {
			tmp_read_bytes = read(efpm_request_body_fd, buffer + read_bytes, count_bytes - read_bytes);
		}
		if (tmp_read_bytes <= 0) {
			break;
		}
		read_bytes += tmp_read_bytes;
	}
	return read_bytes;
}
/* }}} */

static char *sapi_cgi_read_cookies(void) /* {{{ */
{
	fcgi_request *request = (fcgi_request*) SG(server_context);

	return FCGI_GETENV(request, "HTTP_COOKIE");
}
/* }}} */

#define SAPI_CGI_MAX_HEADER_LENGTH 1024

static int sapi_cgi_send_headers(sapi_headers_struct *sapi_headers) /* {{{ */
{
	char buf[SAPI_CGI_MAX_HEADER_LENGTH];
	sapi_header_struct *h;
	zend_llist_position pos;
	bool ignore_status = 0;
	int response_status = SG(sapi_headers).http_response_code;

	if (SG(request_info).no_headers == 1) {
		return  SAPI_HEADER_SENT_SUCCESSFULLY;
	}

	if (CGIG(nph) || SG(sapi_headers).http_response_code != 200)
	{
		int len;
		bool has_status = 0;

		if (CGIG(rfc2616_headers) && SG(sapi_headers).http_status_line) {
			char *s;
			len = slprintf(buf, SAPI_CGI_MAX_HEADER_LENGTH, "%s", SG(sapi_headers).http_status_line);
			if ((s = strchr(SG(sapi_headers).http_status_line, ' '))) {
				response_status = atoi((s + 1));
			}

			if (len > SAPI_CGI_MAX_HEADER_LENGTH) {
				len = SAPI_CGI_MAX_HEADER_LENGTH;
			}

		} else {
			char *s;

			if (SG(sapi_headers).http_status_line &&
				(s = strchr(SG(sapi_headers).http_status_line, ' ')) != 0 &&
				(s - SG(sapi_headers).http_status_line) >= 5 &&
				strncasecmp(SG(sapi_headers).http_status_line, "HTTP/", 5) == 0
			) {
				len = slprintf(buf, sizeof(buf), "Status:%s", s);
				response_status = atoi((s + 1));
			} else {
				h = (sapi_header_struct*)zend_llist_get_first_ex(&sapi_headers->headers, &pos);
				while (h) {
					if (h->header_len > sizeof("Status:") - 1 &&
						strncasecmp(h->header, "Status:", sizeof("Status:") - 1) == 0
					) {
						has_status = 1;
						break;
					}
					h = (sapi_header_struct*)zend_llist_get_next_ex(&sapi_headers->headers, &pos);
				}
				if (!has_status) {
					http_response_status_code_pair *err = (http_response_status_code_pair*)http_status_map;

					while (err->code != 0) {
						if (err->code == SG(sapi_headers).http_response_code) {
							break;
						}
						err++;
					}
					if (err->str) {
						len = slprintf(buf, sizeof(buf), "Status: %d %s", SG(sapi_headers).http_response_code, err->str);
					} else {
						len = slprintf(buf, sizeof(buf), "Status: %d", SG(sapi_headers).http_response_code);
					}
				}
			}
		}

		if (!has_status) {
			PHPWRITE_H(buf, len);
			PHPWRITE_H("\r\n", 2);
			ignore_status = 1;
		}
	}

	h = (sapi_header_struct*)zend_llist_get_first_ex(&sapi_headers->headers, &pos);
	while (h) {
		/* prevent CRLFCRLF */
		if (h->header_len) {
			if (h->header_len > sizeof("Status:") - 1 &&
				strncasecmp(h->header, "Status:", sizeof("Status:") - 1) == 0
			) {
				if (!ignore_status) {
					ignore_status = 1;
					PHPWRITE_H(h->header, h->header_len);
					PHPWRITE_H("\r\n", 2);
				}
			} else if (response_status == 304 && h->header_len > sizeof("Content-Type:") - 1 &&
				strncasecmp(h->header, "Content-Type:", sizeof("Content-Type:") - 1) == 0
			) {
				h = (sapi_header_struct*)zend_llist_get_next_ex(&sapi_headers->headers, &pos);
				continue;
			} else {
				PHPWRITE_H(h->header, h->header_len);
				PHPWRITE_H("\r\n", 2);
			}
		}
		h = (sapi_header_struct*)zend_llist_get_next_ex(&sapi_headers->headers, &pos);
	}
	PHPWRITE_H("\r\n", 2);

	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void sapi_cgibin_flush(void *server_context) /* {{{ */
{
	/* fpm has started, let use fcgi instead of stdout */
	if (efpm_is_running) {
		fcgi_request *request = (fcgi_request*) server_context;
		if (request) {
			sapi_send_headers();
			if (!fcgi_flush(request, 0)) {
				php_handle_aborted_connection();
			}
		}
		return;
	}

	/* fpm has not started yet, let use stdout instead of fcgi */
	if (fflush(stdout) == EOF) {
		php_handle_aborted_connection();
	}
}
/* }}} */

static inline size_t sapi_cgibin_single_write(const char *str, uint32_t str_length) /* {{{ */
{
	ssize_t ret;

	/* sapi has started which means everything must be send through fcgi */
	if (efpm_is_running) {
		fcgi_request *request = (fcgi_request*) SG(server_context);
		ret = fcgi_write(request, FCGI_STDOUT, str, str_length);
		if (ret <= 0) {
			return 0;
		}
		return (size_t)ret;
	}

	/* sapi has not started, output to stdout instead of fcgi */
#ifdef PHP_WRITE_STDOUT
	ret = write(STDOUT_FILENO, str, str_length);
	if (ret <= 0) {
		return 0;
	}
	return (size_t)ret;
#else
	return fwrite(str, 1, MIN(str_length, 16384), stdout);
#endif
}
/* }}} */

static size_t sapi_cgibin_ub_write(const char *str, size_t str_length) /* {{{ */
{
	const char *ptr = str;
	uint32_t remaining = str_length;
	size_t ret;

	while (remaining > 0) {
		ret = sapi_cgibin_single_write(ptr, remaining);
		if (!ret) {
			php_handle_aborted_connection();
			return str_length - remaining;
		}
		ptr += ret;
		remaining -= ret;
	}

	return str_length;
}
/* }}} */

static int php_cgi_startup(struct _sapi_module_struct *sapi_module) {
    return php_module_startup(sapi_module, NULL);
}
/* }}} */

// 필수 인터페이스
/* {{{ sapi_module_struct cgi_sapi_module */
static sapi_module_struct cgi_sapi_module = {
    .name = "efpm-fcgi",    /* name */
    .pretty_name = "EFPM/FastCGI", /* pretty name */

    /* minit startup */
    .startup = php_cgi_startup,
    /* mshutdown shutdown */
    .shutdown = php_module_shutdown_wrapper,
    /* RINIT & RSHUTDOWN */
    .activate = sapi_cgi_active,
    .deactivate = sapi_cgi_deactivate,
    .ub_write = sapi_cgibin_ub_write,
    .flush = sapi_cgibin_flush,
    // .get_stat = NULL, 
    .getenv = sapi_cgibin_getenv,
    .sapi_error = php_error,
    .header_handler = NULL, 
    .send_headers = sapi_cgi_send_headers,
    .send_header = NULL,
    .read_post = sapi_cgi_read_post,
    .read_cookies = sapi_cgi_read_cookies,
    .register_server_variables = sapi_cgi_register_variables, 
    .log_message = sapi_cgi_log_message,
    .get_request_time = NULL, 
    .terminate_process = NULL, 
    // .default_post_reader = NULL, 
    // .treat_data = NULL, 
    // .get_fd = NULL, 
    // .force_http_10 = NULL, 
    // .get_target_uid = NULL,
    // .get_target_gid = NULL,
    // .input_filter = NULL, 
    // .ini_defaults = NULL, 
    // .input_filter_init = NULL, 
    // .pre_request_init = NULL,
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
"html_errors=0\nregister_argc_argv=0\nimplicit_flush=1\noutput_buffering=0\n"
"max_execution_time=0\n";

#define EFPM_PHP_INI_ALTERING_ERROR   -1
#define EFPM_PHP_INI_APPLIED          1
#define EFPM_PHP_INI_EXTENSION_FAILED 0
#define EFPM_PHP_INI_EXTENSION_LOADED 2

static int efpm_php_zend_ini_alter_master(char *name, int name_length, char *new_value, int new_value_length, int mode, int stage) /* {{{ */
{
	zend_ini_entry *ini_entry;
	zend_string *duplicate;

	if ((ini_entry = zend_hash_str_find_ptr(EG(ini_directives), name, name_length)) == NULL) {
		return FAILURE;
	}

	duplicate = zend_string_init(new_value, new_value_length, 1);

	if (!ini_entry->on_modify
			|| ini_entry->on_modify(ini_entry, duplicate,
				ini_entry->mh_arg1, ini_entry->mh_arg2, ini_entry->mh_arg3, stage) == SUCCESS) {
		ini_entry->value = duplicate;
		/* when mode == ZEND_INI_USER keep unchanged to allow ZEND_INI_PERDIR (.user.ini) */
		if (mode == ZEND_INI_SYSTEM) {
			ini_entry->modifiable = mode;
		}
	} else {
		/* The string wasn't installed and won't be shared, it's safe to drop. */
		GC_MAKE_PERSISTENT_LOCAL(duplicate);
		zend_string_release_ex(duplicate, 1);
	}

	return SUCCESS;
}
/* }}} */

int efpm_php_apply_defines_ex(struct key_value_s *kv, int mode) /* {{{ */
{

	char *name = kv->key;
	char *value = kv->value;
	int name_len = strlen(name);
	int value_len = strlen(value);

	if (!strcmp(name, "extension") && *value) {
		zval zv;
		zend_interned_strings_switch_storage(0);

#if ZEND_RC_DEBUG
		bool orig_rc_debug = zend_rc_debug;
		/* Loading extensions after php_module_startup() breaks some invariants.
		 * For instance, it will update the refcount of persistent strings,
		 * which is normally not allowed at this stage. */
		zend_rc_debug = false;
#endif

		php_dl(value, MODULE_PERSISTENT, &zv, 1);

#if ZEND_RC_DEBUG
		zend_rc_debug = orig_rc_debug;
#endif

		zend_interned_strings_switch_storage(1);
		return Z_TYPE(zv) == IS_TRUE ? EFPM_PHP_INI_EXTENSION_LOADED : EFPM_PHP_INI_EXTENSION_FAILED;
	}

	if (efpm_php_zend_ini_alter_master(name, name_len, value, value_len, mode, PHP_INI_STAGE_ACTIVATE) == FAILURE) {
		return EFPM_PHP_INI_ALTERING_ERROR;
	}

	if (!strcmp(name, "disable_functions") && *value) {
		zend_disable_functions(value);
		return EFPM_PHP_INI_APPLIED;
	}

	return EFPM_PHP_INI_APPLIED;
}
/* }}} */

/* {{{ is_valid_path
 *
 * some server configurations allow '..' to slip through in the
 * translated path.   We'll just refuse to handle such a path.
 */
static int is_valid_path(const char *path)
{
	const char *p;

	if (!path) {
		return 0;
	}
	p = strstr(path, "..");
	if (p) {
		if ((p == path || IS_SLASH(*(p-1))) &&
			(*(p+2) == 0 || IS_SLASH(*(p+2)))
		) {
			return 0;
		}
		while (1) {
			p = strstr(p+1, "..");
			if (!p) {
				break;
			}
			if (IS_SLASH(*(p-1)) &&
				(*(p+2) == 0 || IS_SLASH(*(p+2)))
			) {
					return 0;
			}
		}
	}
	return 1;
}
/* }}} */

static void fastcgi_ini_parser(zval *arg1, zval *arg2, zval *arg3, int callback_type, void *arg) /* {{{ */
{
	int *mode = (int *)arg;
	char *key;
	char *value = NULL;
	struct key_value_s kv;

	if (!mode || !arg1) return;

	if (callback_type != ZEND_INI_PARSER_ENTRY) {
		printf("Passing INI directive through FastCGI: only classic entries are allowed\n");
		return;
	}

	key = Z_STRVAL_P(arg1);

	if (!key || strlen(key) < 1) {
		printf("Passing INI directive through FastCGI: empty key\n");
		return;
	}

	if (arg2) {
		value = Z_STRVAL_P(arg2);
	}

	if (!value) {
		printf("Passing INI directive through FastCGI: empty value for key '%s'\n", key);
		return;
	}

	kv.key = key;
	kv.value = value;
	kv.next = NULL;
	if (efpm_php_apply_defines_ex(&kv, *mode) == -1) {
		printf("Passing INI directive through FastCGI: unable to set '%s'\n", key);
	}
}
/* }}} */

static void init_request_info(void)
{
	fcgi_request *request = (fcgi_request*) SG(server_context);
	char *env_script_filename = FCGI_GETENV(request, "SCRIPT_FILENAME");
	char *env_path_translated = FCGI_GETENV(request, "PATH_TRANSLATED");
	char *script_path_translated = env_script_filename;
	char *ini;
	int apache_was_here = 0;

	/* some broken servers do not have script_filename or argv0
	 * an example, IIS configured in some ways.  then they do more
	 * broken stuff and set path_translated to the cgi script location */
	if (!script_path_translated && env_path_translated) {
		script_path_translated = env_path_translated;
	}

	/* initialize the defaults */
	SG(request_info).path_translated = NULL;
	SG(request_info).request_method = FCGI_GETENV(request, "REQUEST_METHOD");
	SG(request_info).proto_num = 1000;
	SG(request_info).query_string = NULL;
	SG(request_info).request_uri = NULL;
	SG(request_info).content_type = NULL;
	SG(request_info).content_length = 0;
	SG(sapi_headers).http_response_code = 200;

	/* if script_path_translated is not set, then there is no point to carry on
	 * as the response is 404 and there is no further processing. */
	if (script_path_translated) {
		const char *auth;
		char *content_length = FCGI_GETENV(request, "CONTENT_LENGTH");
		char *content_type = FCGI_GETENV(request, "CONTENT_TYPE");
		char *env_path_info = FCGI_GETENV(request, "PATH_INFO");
		char *env_script_name = FCGI_GETENV(request, "SCRIPT_NAME");

		/* Hack for buggy IIS that sets incorrect PATH_INFO */
		char *env_server_software = FCGI_GETENV(request, "SERVER_SOFTWARE");
		if (env_server_software &&
			env_script_name &&
			env_path_info &&
			strncmp(env_server_software, "Microsoft-IIS", sizeof("Microsoft-IIS") - 1) == 0 &&
			strncmp(env_path_info, env_script_name, strlen(env_script_name)) == 0
		) {
			env_path_info = FCGI_PUTENV(request, "ORIG_PATH_INFO", env_path_info);
			env_path_info += strlen(env_script_name);
			if (*env_path_info == 0) {
				env_path_info = NULL;
			}
			env_path_info = FCGI_PUTENV(request, "PATH_INFO", env_path_info);
		}

#define APACHE_PROXY_FCGI_PREFIX "proxy:fcgi://"
#define APACHE_PROXY_BALANCER_PREFIX "proxy:balancer://"
		/* Fix proxy URLs in SCRIPT_FILENAME generated by Apache mod_proxy_fcgi and mod_proxy_balancer:
		 *     proxy:fcgi://localhost:9000/some-dir/info.php/test?foo=bar
		 *     proxy:balancer://localhost:9000/some-dir/info.php/test?foo=bar
		 * should be changed to:
		 *     /some-dir/info.php/test
		 * See: http://bugs.php.net/bug.php?id=54152
		 *      http://bugs.php.net/bug.php?id=62172
		 *      https://issues.apache.org/bugzilla/show_bug.cgi?id=50851
		 */
		if (env_script_filename &&
			strncasecmp(env_script_filename, APACHE_PROXY_FCGI_PREFIX, sizeof(APACHE_PROXY_FCGI_PREFIX) - 1) == 0) {
			/* advance to first character of hostname */
			char *p = env_script_filename + (sizeof(APACHE_PROXY_FCGI_PREFIX) - 1);
			while (*p != '\0' && *p != '/') {
				p++;	/* move past hostname and port */
			}
			if (*p != '\0') {
				/* Copy path portion in place to avoid memory leak.  Note
				 * that this also affects what script_path_translated points
				 * to. */
				memmove(env_script_filename, p, strlen(p) + 1);
				apache_was_here = 1;
			}
			/* ignore query string if sent by Apache (RewriteRule) */
			p = strchr(env_script_filename, '?');
			if (p) {
				*p =0;
			}
		}

		if (env_script_filename &&
			strncasecmp(env_script_filename, APACHE_PROXY_BALANCER_PREFIX, sizeof(APACHE_PROXY_BALANCER_PREFIX) - 1) == 0) {
			/* advance to first character of hostname */
			char *p = env_script_filename + (sizeof(APACHE_PROXY_BALANCER_PREFIX) - 1);
			while (*p != '\0' && *p != '/') {
				p++;	/* move past hostname and port */
			}
			if (*p != '\0') {
				/* Copy path portion in place to avoid memory leak.  Note
				 * that this also affects what script_path_translated points
				 * to. */
				memmove(env_script_filename, p, strlen(p) + 1);
				apache_was_here = 1;
			}
			/* ignore query string if sent by Apache (RewriteRule) */
			p = strchr(env_script_filename, '?');
			if (p) {
				*p =0;
			}
		}

		if (apache_was_here && !CGIG(fcgi_script_path_encoded)) {
			php_raw_url_decode(env_script_filename, strlen(env_script_filename));
		}

		if (CGIG(fix_pathinfo)) {
			struct stat st;
			char *real_path = NULL;
			char *env_redirect_url = FCGI_GETENV(request, "REDIRECT_URL");
			char *env_document_root = FCGI_GETENV(request, "DOCUMENT_ROOT");
			char *orig_path_translated = env_path_translated;
			char *orig_path_info = env_path_info;
			char *orig_script_name = env_script_name;
			char *orig_script_filename = env_script_filename;
			int script_path_translated_len;

			if (!env_document_root && PG(doc_root)) {
				env_document_root = FCGI_PUTENV(request, "DOCUMENT_ROOT", PG(doc_root));
			}

			if (!apache_was_here && env_path_translated != NULL && env_redirect_url != NULL &&
			    env_path_translated != script_path_translated &&
			    strcmp(env_path_translated, script_path_translated) != 0) {
				/*
				 * pretty much apache specific.  If we have a redirect_url
				 * then our script_filename and script_name point to the
				 * php executable
				 * we don't want to do this for the new mod_proxy_fcgi approach,
				 * where redirect_url may also exist but the below will break
				 * with rewrites to PATH_INFO, hence the !apache_was_here check
				 */
				script_path_translated = env_path_translated;
				/* we correct SCRIPT_NAME now in case we don't have PATH_INFO */
				env_script_name = env_redirect_url;
			}

#ifdef __riscos__
			/* Convert path to unix format*/
			__riscosify_control |= __RISCOSIFY_DONT_CHECK_DIR;
			script_path_translated = __unixify(script_path_translated, 0, NULL, 1, 0);
#endif

			/*
			 * if the file doesn't exist, try to extract PATH_INFO out
			 * of it by stat'ing back through the '/'
			 * this fixes url's like /info.php/test
			 */
			if (script_path_translated &&
				(script_path_translated_len = strlen(script_path_translated)) > 0 &&
				(script_path_translated[script_path_translated_len-1] == '/' ||
				(real_path = tsrm_realpath(script_path_translated, NULL)) == NULL)
			) {
				char *pt = estrndup(script_path_translated, script_path_translated_len);
				int len = script_path_translated_len;
				char *ptr;

				if (pt) {
					while ((ptr = strrchr(pt, '/')) || (ptr = strrchr(pt, '\\'))) {
						*ptr = 0;
						if (stat(pt, &st) == 0 && S_ISREG(st.st_mode)) {
							/*
							 * okay, we found the base script!
							 * work out how many chars we had to strip off;
							 * then we can modify PATH_INFO
							 * accordingly
							 *
							 * we now have the makings of
							 * PATH_INFO=/test
							 * SCRIPT_FILENAME=/docroot/info.php
							 *
							 * we now need to figure out what docroot is.
							 * if DOCUMENT_ROOT is set, this is easy, otherwise,
							 * we have to play the game of hide and seek to figure
							 * out what SCRIPT_NAME should be
							 */
							int ptlen = strlen(pt);
							int slen = len - ptlen;
							int pilen = env_path_info ? strlen(env_path_info) : 0;
							int tflag = 0;
							char *path_info;
							if (apache_was_here) {
								/* recall that PATH_INFO won't exist */
								path_info = script_path_translated + ptlen;
								tflag = (slen != 0 && (!orig_path_info || strcmp(orig_path_info, path_info) != 0));
							} else {
								path_info = (env_path_info && pilen > slen) ? env_path_info + pilen - slen : NULL;
								tflag = path_info && (orig_path_info != path_info);
							}

							if (tflag) {
								char *decoded_path_info = NULL;
								if (orig_path_info) {
									char old;

									FCGI_PUTENV(request, "ORIG_PATH_INFO", orig_path_info);
									old = path_info[0];
									path_info[0] = 0;
									if (!orig_script_name ||
										strcmp(orig_script_name, env_path_info) != 0) {
										if (orig_script_name) {
											FCGI_PUTENV(request, "ORIG_SCRIPT_NAME", orig_script_name);
										}
										SG(request_info).request_uri = FCGI_PUTENV(request, "SCRIPT_NAME", env_path_info);
									} else {
										SG(request_info).request_uri = orig_script_name;
									}
									path_info[0] = old;
								} else if (apache_was_here && env_script_name) {
									/* Using mod_proxy_fcgi and ProxyPass, apache cannot set PATH_INFO
									 * As we can extract PATH_INFO from PATH_TRANSLATED
									 * it is probably also in SCRIPT_NAME and need to be removed
									 */
									size_t decoded_path_info_len = 0;
									if (CGIG(fcgi_script_path_encoded) && strchr(path_info, '%')) {
										decoded_path_info = estrdup(path_info);
										decoded_path_info_len = php_raw_url_decode(decoded_path_info, strlen(path_info));
									}
									size_t snlen = strlen(env_script_name);
									size_t env_script_file_info_start = 0;
									if (
										(
											snlen > slen &&
											!strcmp(env_script_name + (env_script_file_info_start = snlen - slen), path_info)
										) ||
										(
											decoded_path_info &&
											snlen > decoded_path_info_len &&
											!strcmp(env_script_name + (env_script_file_info_start = snlen - decoded_path_info_len), decoded_path_info)
										)
									) {
										FCGI_PUTENV(request, "ORIG_SCRIPT_NAME", orig_script_name);
										env_script_name[env_script_file_info_start] = 0;
										SG(request_info).request_uri = FCGI_PUTENV(request, "SCRIPT_NAME", env_script_name);
									}
								}
								if (decoded_path_info) {
									env_path_info = FCGI_PUTENV(request, "PATH_INFO", decoded_path_info);
									efree(decoded_path_info);
								} else {
									env_path_info = FCGI_PUTENV(request, "PATH_INFO", path_info);
								}
							}
							if (!orig_script_filename ||
								strcmp(orig_script_filename, pt) != 0) {
								if (orig_script_filename) {
									FCGI_PUTENV(request, "ORIG_SCRIPT_FILENAME", orig_script_filename);
								}
								script_path_translated = FCGI_PUTENV(request, "SCRIPT_FILENAME", pt);
							}

							/* figure out docroot
							 * SCRIPT_FILENAME minus SCRIPT_NAME
							 */
							if (env_document_root) {
								int l = strlen(env_document_root);
								int path_translated_len = 0;
								char *path_translated = NULL;

								if (l && env_document_root[l - 1] == '/') {
									--l;
								}

								/* we have docroot, so we should have:
								 * DOCUMENT_ROOT=/docroot
								 * SCRIPT_FILENAME=/docroot/info.php
								 */

								/* PATH_TRANSLATED = DOCUMENT_ROOT + PATH_INFO */
								path_translated_len = l + (env_path_info ? strlen(env_path_info) : 0);
								path_translated = (char *) emalloc(path_translated_len + 1);
								memcpy(path_translated, env_document_root, l);
								if (env_path_info) {
									memcpy(path_translated + l, env_path_info, (path_translated_len - l));
								}
								path_translated[path_translated_len] = '\0';
								if (orig_path_translated) {
									FCGI_PUTENV(request, "ORIG_PATH_TRANSLATED", orig_path_translated);
								}
								env_path_translated = FCGI_PUTENV(request, "PATH_TRANSLATED", path_translated);
								efree(path_translated);
							} else if (	env_script_name &&
										strstr(pt, env_script_name)
							) {
								/* PATH_TRANSLATED = PATH_TRANSLATED - SCRIPT_NAME + PATH_INFO */
								int ptlen = strlen(pt) - strlen(env_script_name);
								int path_translated_len = ptlen + (env_path_info ? strlen(env_path_info) : 0);
								char *path_translated = NULL;

								path_translated = (char *) emalloc(path_translated_len + 1);
								memcpy(path_translated, pt, ptlen);
								if (env_path_info) {
									memcpy(path_translated + ptlen, env_path_info, path_translated_len - ptlen);
								}
								path_translated[path_translated_len] = '\0';
								if (orig_path_translated) {
									FCGI_PUTENV(request, "ORIG_PATH_TRANSLATED", orig_path_translated);
								}
								env_path_translated = FCGI_PUTENV(request, "PATH_TRANSLATED", path_translated);
								efree(path_translated);
							}
							break;
						}
					}
				} else {
					ptr = NULL;
				}
				if (!ptr) {
					/*
					 * if we stripped out all the '/' and still didn't find
					 * a valid path... we will fail, badly. of course we would
					 * have failed anyway... we output 'no input file' now.
					 */
					if (orig_script_filename) {
						FCGI_PUTENV(request, "ORIG_SCRIPT_FILENAME", orig_script_filename);
					}
					script_path_translated = FCGI_PUTENV(request, "SCRIPT_FILENAME", NULL);
					SG(sapi_headers).http_response_code = 404;
				}
				if (!SG(request_info).request_uri) {
					if (!orig_script_name ||
						strcmp(orig_script_name, env_script_name) != 0) {
						if (orig_script_name) {
							FCGI_PUTENV(request, "ORIG_SCRIPT_NAME", orig_script_name);
						}
						SG(request_info).request_uri = FCGI_PUTENV(request, "SCRIPT_NAME", env_script_name);
					} else {
						SG(request_info).request_uri = orig_script_name;
					}
				}
				if (pt) {
					efree(pt);
				}
			} else {
				/* make sure original values are remembered in ORIG_ copies if we've changed them */
				if (!orig_script_filename ||
					(script_path_translated != orig_script_filename &&
					strcmp(script_path_translated, orig_script_filename) != 0)) {
					if (orig_script_filename) {
						FCGI_PUTENV(request, "ORIG_SCRIPT_FILENAME", orig_script_filename);
					}
					script_path_translated = FCGI_PUTENV(request, "SCRIPT_FILENAME", script_path_translated);
				}
				if (!apache_was_here && env_redirect_url) {
					/* if we used PATH_TRANSLATED to work around Apache mod_fastcgi (but not mod_proxy_fcgi,
					 * hence !apache_was_here) weirdness, strip info accordingly */
					if (orig_path_info) {
						FCGI_PUTENV(request, "ORIG_PATH_INFO", orig_path_info);
						FCGI_PUTENV(request, "PATH_INFO", NULL);
					}
					if (orig_path_translated) {
						FCGI_PUTENV(request, "ORIG_PATH_TRANSLATED", orig_path_translated);
						FCGI_PUTENV(request, "PATH_TRANSLATED", NULL);
					}
				}
				if (env_script_name != orig_script_name) {
					if (orig_script_name) {
						FCGI_PUTENV(request, "ORIG_SCRIPT_NAME", orig_script_name);
					}
					SG(request_info).request_uri = FCGI_PUTENV(request, "SCRIPT_NAME", env_script_name);
				} else {
					SG(request_info).request_uri = env_script_name;
				}
				efree(real_path);
			}
		} else {
			/* pre 4.3 behaviour, shouldn't be used but provides BC */
			if (env_path_info) {
				SG(request_info).request_uri = env_path_info;
			} else {
				SG(request_info).request_uri = env_script_name;
			}
			if (!CGIG(discard_path) && env_path_translated) {
				script_path_translated = env_path_translated;
			}
		}

		if (is_valid_path(script_path_translated)) {
			SG(request_info).path_translated = estrdup(script_path_translated);
		}

		/* FIXME - Work out proto_num here */
		SG(request_info).query_string = FCGI_GETENV(request, "QUERY_STRING");
		SG(request_info).content_type = (content_type ? content_type : "" );
		SG(request_info).content_length = (content_length ? atol(content_length) : 0);

		/* The CGI RFC allows servers to pass on unvalidated Authorization data */
		auth = FCGI_GETENV(request, "HTTP_AUTHORIZATION");
		php_handle_auth_data(auth);
	}

	/* INI stuff */
	ini = FCGI_GETENV(request, "PHP_VALUE");
	if (ini) {
		int mode = ZEND_INI_USER;
		char *tmp;
		spprintf(&tmp, 0, "%s\n", ini);
		zend_parse_ini_string(tmp, 1, ZEND_INI_SCANNER_NORMAL, (zend_ini_parser_cb_t)fastcgi_ini_parser, &mode);
		efree(tmp);
	}

	ini = FCGI_GETENV(request, "PHP_ADMIN_VALUE");
	if (ini) {
		int mode = ZEND_INI_SYSTEM;
		char *tmp;
		spprintf(&tmp, 0, "%s\n", ini);
		zend_parse_ini_string(tmp, 1, ZEND_INI_SCANNER_NORMAL, (zend_ini_parser_cb_t)fastcgi_ini_parser, &mode);
		efree(tmp);
	}
}
/* }}} */

void efpm_handle_client(struct efpm_event_s *ev, uint32_t flags, void *arg) {
    struct efpm_s *this = efpm_g;
    fcgi_request *request = (fcgi_request *)arg;
    struct sockaddr_in sa;
	char *primary_script = NULL;
	zend_file_handle file_handle;
	int exit_status = EFPM_EXIT_OK;
	int clnt_fd = fcgi_get_fd(request);

    sa = fcgi_get_sockaddr(request);
	printf("req from IP: %s, FD: %d, PID: %d\n", fcgi_get_client_ip(sa), fcgi_get_fd(request), getpid());

    int ret = fcgi_process_request(request); 
    if(ret == -1) {
        printf("failed to fcgi_process_request()\n");
        return;
    }

    efpm_request_body_fd = -1;
    SG(server_context) = (void*)request;
    CGIG(fcgi_logging_request_started) = false;
	init_request_info();

	php_request_startup();
	if(!SG(request_info).request_method) {
		printf("invalid request method\n");
		return;
	}

	if(!SG(request_info).path_translated) {
		printf("invalid path translated");
		return;
	}

	primary_script = SG(request_info).path_translated;
	if(php_fopen_primary_script(&file_handle) == FAILURE) {
		printf("failed to php_fopen_primary_script()\n");
		return;
	}

	ev->fd = clnt_fd;
	if((*this->event_module->remove)(this->event_module, ev) == FAILURE){
		printf("failed to remove handled request\n");
	}

	// 여기만 알면되지? php_execute_script만 따로 스레딩 ? 

	// php script
	EG(exit_status) = 0;
	php_execute_script(&file_handle);

	efree(primary_script);
	efree(SG(request_info).path_translated);
	if(!file_handle.in_list) {
		zend_destroy_file_handle(&file_handle);
	}

	if(efpm_request_body_fd != -1) {
		close(efpm_request_body_fd);
	}

	efpm_request_body_fd = -2;
	if(EG(exit_status) == 255) {
		printf("exit status code is 255\n");
	}

	fcgi_finish_request(request, 0);
	fcgi_destroy_request(request);

	SG(request_info).path_translated = NULL;
	php_request_shutdown((void*)0);
    return;
}

/* {{{ main */
int main(int argc, char **argv) {
    printf("starting PHP-EFPM\n");
    zend_file_handle file_handle;
    zend_signal_startup();
    sapi_startup(&cgi_sapi_module); // php_module_startup 포함됨.

    fcgi_init();

    cgi_sapi_module.ini_entries = malloc(sizeof(HARDCODED_INI));
    memcpy((void*)cgi_sapi_module.ini_entries, HARDCODED_INI, sizeof(HARDCODED_INI));

    cgi_sapi_module.additional_functions = NULL;
    cgi_sapi_module.executable_location = argv[0];
    if(cgi_sapi_module.startup(&cgi_sapi_module) == FAILURE) {
        return EFPM_EXIT_SOFTWARE;
    }

	struct efpm_s *efpm = new_efpm();
	if(!efpm){
		return EFPM_EXIT_SOFTWARE;
	}

	if((*efpm->init)(efpm) == FAILURE){
		return EFPM_EXIT_SOFTWARE;
	}

	if((*efpm->run)(efpm) == FAILURE) {
		return EFPM_EXIT_SOFTWARE;
	}

	fcgi_shutdown();
	SG(server_context) = NULL;
	php_module_shutdown();
	sapi_shutdown();

	return 0;
}
/* }}} */