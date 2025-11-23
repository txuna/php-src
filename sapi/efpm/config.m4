dnl ======================================================================
dnl  EFPM (Epoll-based FastCGI Process Manager) SAPI
dnl  Directory layout (in php-src):
dnl    sapi/efpm/config.m4
dnl    sapi/efpm/Makefile.frag   (a.k.a. Makefile fragment)
dnl    sapi/efpm/efpm/efpm_main.c
dnl    sapi/efpm/efpm/efpm.c
dnl    sapi/efpm/efpm/efpm_event.c
dnl ======================================================================

PHP_ARG_ENABLE([efpm],
  [for EFPM build],
  [AS_HELP_STRING([--enable-efpm],
    [Enable building of the efpm SAPI executable (epoll-driven)])],
  [no],
  [no])

dnl -------- Optional feature/OS checks kept minimal, tailored to epoll --------

AC_DEFUN([PHP_EFPM_EPOLL], [
  AC_CACHE_CHECK([for epoll], [php_cv_have_epoll],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <sys/epoll.h>]], [[
      int epfd = epoll_create(1);
      return (epfd < 0);
    ]])],
    [php_cv_have_epoll=yes],
    [php_cv_have_epoll=no])])
  AS_VAR_IF([php_cv_have_epoll], [yes],
    [AC_DEFINE([HAVE_EPOLL], [1], [Define to 1 if system has a working epoll.])],
    [AC_MSG_FAILURE([efpm requires epoll(7) support on this platform.])])
])

AC_DEFUN([PHP_EFPM_CLOCK], [
  AC_CHECK_FUNCS([clock_gettime],,
  [LIBS_save=$LIBS
   AC_SEARCH_LIBS([clock_gettime], [rt], [
     ac_cv_func_clock_gettime=yes
     AC_DEFINE([HAVE_CLOCK_GETTIME], [1], [Have clock_gettime()])
     AS_VAR_IF([ac_cv_search_clock_gettime], ["none required"],,
       [AS_VAR_APPEND([EFPM_EXTRA_LIBS], [" $ac_cv_search_clock_gettime"])])
   ])
   LIBS=$LIBS_save])
])

AC_DEFUN([PHP_EFPM_SELECT], [
  AC_CHECK_FUNCS([select],,
    [AC_MSG_WARN([select() not found; continuing, but some fallbacks may be disabled])])
])

AC_DEFUN([PHP_EFPM_BUILTIN_ATOMIC], [
  AC_CACHE_CHECK([if compiler supports __sync_bool_compare_and_swap],
    [php_cv_have___sync_bool_compare_and_swap],
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([], [[
      int v=1;
      return (__sync_bool_compare_and_swap(&v,1,2) && __sync_add_and_fetch(&v,1)) ? 0 : 1;
    ]])],
    [php_cv_have___sync_bool_compare_and_swap=yes],
    [php_cv_have___sync_bool_compare_and_swap=no])])
  AS_VAR_IF([php_cv_have___sync_bool_compare_and_swap], [yes],
    [AC_DEFINE([HAVE_BUILTIN_ATOMIC], [1],
      [Define to 1 if compiler supports __sync_* builtins])])
])

dnl --------------------------- main configure body ---------------------------

if test "$PHP_EFPM" != "no"; then
  PHP_EFPM_EPOLL
  PHP_EFPM_CLOCK
  PHP_EFPM_SELECT
  PHP_EFPM_BUILTIN_ATOMIC

  dnl Build dirs (so that generated .lo/.o go under these)
  PHP_ADD_BUILD_DIR([sapi/efpm])
  PHP_ADD_BUILD_DIR([sapi/efpm/efpm])

  dnl Source files for efpm SAPI
  PHP_EFPM_FILES="efpm/efpm.c \
    efpm/efpm_event.c \
    efpm/efpm_main.c \
  "

  dnl Select SAPI: program type (builds an executable)
  PHP_SELECT_SAPI([efpm],
    [program],
    [$PHP_EFPM_FILES],
    [-I$abs_srcdir/sapi/efpm -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])

  dnl Link command per-platform (mirrors sapi/fpm style but simplified)
  AS_CASE([$host_alias],
    [*aix*], [
      BUILD_EFPM="echo '\#! .' > php.sym && echo >> php.sym && nm -BCpg \`echo \$(PHP_GLOBAL_OBJS) \$(PHP_BINARY_OBJS) \$(PHP_FASTCGI_OBJS) \$(PHP_EFPM_OBJS) | sed 's/$begin:math:text$[A-Za-z0-9_]*$end:math:text$\.lo/\1.o/g'\` | \$(AWK) '{ if (((\$\$2 == \"T\") || (\$\$2 == \"D\") || (\$\$2 == \"B\")) && (substr(\$\$3,1,1) != \".\")) { print \$\$3 } }' | sort -u >> php.sym && \$(LIBTOOL) --tag=CC --mode=link \$(CC) -export-dynamic \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) -Wl,-brtl -Wl,-bE:php.sym \$(PHP_RPATHS) \$(PHP_GLOBAL_OBJS) \$(PHP_BINARY_OBJS) \$(PHP_FASTCGI_OBJS) \$(PHP_EFPM_OBJS) \$(EXTRA_LIBS) \$(EFPM_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_EFPM_PATH)"
    ],
    [*darwin*], [
      BUILD_EFPM="\$(CC) \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) \$(NATIVE_RPATHS) \$(PHP_GLOBAL_OBJS:.lo=.o) \$(PHP_BINARY_OBJS:.lo=.o) \$(PHP_FASTCGI_OBJS:.lo=.o) \$(PHP_EFPM_OBJS:.lo=.o) \$(PHP_FRAMEWORKS) \$(EXTRA_LIBS) \$(EFPM_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_EFPM_PATH)"
    ], [
      BUILD_EFPM="\$(LIBTOOL) --tag=CC --mode=link \$(CC) -export-dynamic \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) \$(PHP_RPATHS) \$(PHP_GLOBAL_OBJS:.lo=.o) \$(PHP_BINARY_OBJS:.lo=.o) \$(PHP_FASTCGI_OBJS:.lo=.o) \$(PHP_EFPM_OBJS:.lo=.o) \$(EXTRA_LIBS) \$(EFPM_EXTRA_LIBS) \$(ZEND_EXTRA_LIBS) -o \$(SAPI_EFPM_PATH)"
    ])

  dnl Path for the produced binary
  SAPI_EFPM_PATH=sapi/efpm/php-efpm

  PHP_SUBST([SAPI_EFPM_PATH])
  PHP_SUBST([BUILD_EFPM])
  PHP_SUBST([EFPM_EXTRA_LIBS])

  dnl Provide a makefile fragment for top-level Makefile to include
  PHP_ADD_MAKEFILE_FRAGMENT([$abs_srcdir/sapi/efpm/Makefile.frag])
fi