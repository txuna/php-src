// #include "ext/standard/php_standard.h"
#include <stdio.h>

/*
./buildconf —force
./configure --enable-debug --disable-cgi --enable-efpm --disable-fpm --disable-phpdbg
./config.nice
make -j $(nproc)
sudo make install

php-efpm
*/

/* {{{ main */
int main(int args, char **argv) {
    // PHPWRITE("Hello", 5);
    printf("Hello World\n");
    return 0;
}
/* }}} */
