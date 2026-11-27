/* A web server for the Servers menu: nginx, with PHP through php-fpm, run as the signed-in user.
 * Pixi writes the settings file once and never overwrites it; the page is edited like any file. */
#ifndef PIXI_WEB_H
#define PIXI_WEB_H

#include <stddef.h>

typedef struct {
    int running;      /* nginx is up */
    int php;          /* php-fpm is up too */
    int port;         /* what the settings file listens on */
    char root[512];   /* the folder it serves */
    char php_version[24];
    char last[200];   /* the last line of its error log, if any */
} WebStatus;

int web_installed(void);                     /* nginx is on this system */
int web_php_available(char *version, size_t cap); /* php-fpm is, and which version */
const char *web_config_path(void);           /* ~/.config/pixi/nginx/nginx.conf */
const char *web_root_path(void);             /* ~/www */
int web_start(const char *home, int port, char *err, size_t cap);
void web_stop(void);
int web_running(void);
void web_status(WebStatus *st);
void web_reload(void);                       /* ask it to read the settings file again */
int web_config_test(char *out, size_t cap);  /* nginx -t; 0 when the file is good */
int web_config_ensure(const char *home, int port); /* make the folders and write the file if it isn't there */
int web_config_reset(const char *home, int port); /* write the default settings file again */

#endif
