/* nginx and php-fpm as the signed-in user. Ports below 1024 need the installer's sysctl setting (see web_start). */
#define _GNU_SOURCE
#include "web.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

static struct {
    pid_t nginx, php;
    char home[400], conf[480], run[480], www[480], nginx_bin[200], php_bin[200];
} W;

static void paths(const char *home)
{
    if (W.home[0] && !strcmp(W.home, home)) return;
    snprintf(W.home, sizeof W.home, "%s", home);
    snprintf(W.conf, sizeof W.conf, "%s/.config/pixi/nginx/nginx.conf", home);
    snprintf(W.run, sizeof W.run, "%s/.cache/pixi/nginx", home);
    snprintf(W.www, sizeof W.www, "%s/www", home);
}

static const char *home_of(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : "/root";
}

const char *web_config_path(void)
{
    paths(home_of());
    return W.conf;
}

const char *web_root_path(void)
{
    paths(home_of());
    return W.www;
}

static int find_one(const char *const *list, char *out, size_t cap)
{
    for (; *list; list++)
        if (access(*list, X_OK) == 0) {
            snprintf(out, cap, "%s", *list);
            return 1;
        }
    return 0;
}

int web_installed(void)
{
    static const char *const bins[] = { "/usr/sbin/nginx", "/usr/bin/nginx", "/usr/local/sbin/nginx", "/usr/local/nginx/sbin/nginx", NULL };
    return find_one(bins, W.nginx_bin, sizeof W.nginx_bin);
}

int web_php_available(char *version, size_t cap)
{
    glob_t g;
    int ok = 0;
    if (glob("/usr/sbin/php-fpm[0-9]*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !ok; i++) {
            const char *p = g.gl_pathv[i];
            if (access(p, X_OK)) continue;
            snprintf(W.php_bin, sizeof W.php_bin, "%s", p);
            const char *v = strstr(p, "php-fpm");
            if (version) snprintf(version, cap, "%s", v ? v + 7 : "");
            ok = 1;
        }
        globfree(&g);
    }
    return ok;
}

static void mkdirs(const char *path)
{
    char b[600];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0755);
            *p = '/';
        }
    mkdir(b, 0755);
}

static int write_default(const char *home, int port)
{
    paths(home);
    char dir[480];
    snprintf(dir, sizeof dir, "%s/.config/pixi/nginx", home);
    mkdirs(dir);
    FILE *f = fopen(W.conf, "w");
    if (!f) return -1;
    fprintf(f,
            "# Pixi's web server (nginx). Edit this file, save it, and the server loads it.\n"
            "# It runs as you. Port 80 works because the installer allows ports from 80 up.\n"
            "# Pages go in %s/www.\n"
            "# pixi-web 2\n"
            "\n"
            "worker_processes 1;\n"
            "daemon off;\n"
            "pid %s/nginx.pid;\n"
            "error_log %s/error.log warn;\n"
            "\n"
            "events {\n"
            "    worker_connections 256;\n"
            "}\n"
            "\n"
            "http {\n"
            "    include /etc/nginx/mime.types;\n"
            "    default_type application/octet-stream;\n"
            "    access_log %s/access.log;\n"
            "    sendfile on;\n"
            "    client_max_body_size 64m;\n"
            "    client_body_temp_path %s/tmp/body;\n"
            "    proxy_temp_path %s/tmp/proxy;\n"
            "    fastcgi_temp_path %s/tmp/fastcgi;\n"
            "    uwsgi_temp_path %s/tmp/uwsgi;\n"
            "    scgi_temp_path %s/tmp/scgi;\n"
            "\n"
            "    server {\n"
            "        listen %d;\n"
            "        server_name _;\n"
            "\n"
            "        root %s/www;\n"
            "        index index.php index.html index.htm;\n"
            "\n"
            "        location / {\n"
            "            try_files $uri $uri/ =404;\n"
            "        }\n"
            "\n"
            "        # PHP, through php-fpm (installed with the web server)\n"
            "        location ~ \\.php$ {\n"
            "            include /etc/nginx/fastcgi_params;\n"
            "            fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;\n"
            "            fastcgi_pass unix:%s/php.sock;\n"
            "        }\n"
            "\n"
            "        location ~ /\\.ht {\n"
            "            deny all;\n"
            "        }\n"
            "    }\n"
            "}\n",
            home, W.run, W.run, W.run, W.run, W.run, W.run, W.run, W.run, port, home, W.run);
    fclose(f);
    return 0;
}

static void prepare_dirs(const char *home);

/* A file from before port 80 has no "pixi-web 2" stamp and says listen 8080: move that one line to 80, once.
 * Nothing else in the file is touched, and a file that has the stamp is never changed. */
static void migrate(int port)
{
    if (port != 80) return;
    FILE *f = fopen(W.conf, "r");
    if (!f) return;
    char *buf = malloc(1 << 20);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, (1 << 20) - 64, f);
    fclose(f);
    buf[n] = 0;
    if (strstr(buf, "# pixi-web 2")) {
        free(buf);
        return;
    }
    char *at = strstr(buf, "listen 8080;");
    FILE *o = fopen(W.conf, "w");
    if (o) {
        fputs("# pixi-web 2\n", o);
        if (at) {
            fwrite(buf, 1, (size_t)(at - buf), o);
            fputs("listen 80;", o);
            fputs(at + strlen("listen 8080;"), o);
        } else fputs(buf, o);
        fclose(o);
    }
    free(buf);
}

int web_config_reset(const char *home, int port) { return write_default(home, port); }

int web_config_ensure(const char *home, int port)
{
    paths(home);
    prepare_dirs(home);
    if (access(W.conf, R_OK) != 0) return write_default(home, port);
    migrate(port);
    return 0;
}

/* the lowest port a normal user may listen on (the installer sets this to 80) */
static int unprivileged_start(void)
{
    int v = 1024;
    FILE *f = fopen("/proc/sys/net/ipv4/ip_unprivileged_port_start", "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = 1024;
        fclose(f);
    }
    return v;
}

static int config_port(void)
{
    FILE *f = fopen(web_config_path(), "r");
    if (!f) return 0;
    char line[300];
    int port = 0;
    while (!port && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "listen", 6) || !isspace((unsigned char)p[6])) continue;
        p += 6;
        char *colon = strrchr(p, ':');
        char *q = colon ? colon + 1 : p;
        while (*q == ' ') q++;
        if (isdigit((unsigned char)*q)) port = atoi(q);
    }
    fclose(f);
    return port;
}

static void log_tail(const char *path, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[400], last[400] = "";
    while (fgets(line, sizeof line, f))
        if (line[0] != '\n') snprintf(last, sizeof last, "%s", line);
    fclose(f);
    size_t n = strlen(last);
    while (n && (last[n - 1] == '\n' || last[n - 1] == '\r')) last[--n] = 0;
    snprintf(out, cap, "%s", last);
}

static pid_t spawn(char *const argv[], const char *logfile)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    setsid();
    int fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) dup2(nul, 0);
    if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
    for (int i = 3; i < 256; i++) close(i);
    execv(argv[0], argv);
    _exit(127);
}

/* a server left running by a shell that crashed: ask it to stop, so the port is free */
static void stale_stop(const char *pidfile, const char *comm_prefix)
{
    FILE *f = fopen(pidfile, "r");
    if (!f) return;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 1) return;
    char p[64], name[64] = "";
    snprintf(p, sizeof p, "/proc/%d/comm", pid);
    f = fopen(p, "r");
    if (!f) return;
    if (fgets(name, sizeof name, f)) {}
    fclose(f);
    if (!strncmp(name, comm_prefix, strlen(comm_prefix))) {
        kill(pid, SIGQUIT);
        for (int i = 0; i < 20 && kill(pid, 0) == 0; i++) usleep(100000);
    }
}

static int alive(pid_t *p)
{
    if (*p <= 0) return 0;
    int st;
    pid_t r = waitpid(*p, &st, WNOHANG);
    if (r == *p || (r < 0 && errno == ECHILD)) {
        *p = 0;
        return 0;
    }
    return 1;
}

int web_running(void) { return alive(&W.nginx); }

static void write_php_conf(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
            "[global]\n"
            "pid = %s/php-fpm.pid\n"
            "error_log = %s/php-fpm.log\n"
            "daemonize = no\n"
            "\n"
            "[www]\n"
            "listen = %s/php.sock\n"
            "pm = ondemand\n"
            "pm.max_children = 4\n"
            "pm.process_idle_timeout = 20s\n"
            "catch_workers_output = yes\n"
            "php_admin_value[error_log] = %s/php-error.log\n"
            "php_admin_flag[log_errors] = on\n",
            W.run, W.run, W.run, W.run);
    fclose(f);
}

static void prepare_dirs(const char *home)
{
    paths(home);
    mkdirs(W.run);
    static const char *const tmp[] = { "tmp", "tmp/body", "tmp/proxy", "tmp/fastcgi", "tmp/uwsgi", "tmp/scgi" };
    for (size_t i = 0; i < sizeof tmp / sizeof *tmp; i++) {
        char d[540];
        snprintf(d, sizeof d, "%s/%s", W.run, tmp[i]);
        mkdir(d, 0755);
    }
    mkdir(W.www, 0755);
    char idx[540];
    snprintf(idx, sizeof idx, "%s/index.html", W.www);
    char php[540];
    snprintf(php, sizeof php, "%s/index.php", W.www);
    if (access(idx, F_OK) && access(php, F_OK)) { /* a first page, so there's something to see */
        FILE *f = fopen(idx, "w");
        if (f) {
            fputs("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                  "<title>Pixi web server</title>\n"
                  "<style>body{font-family:system-ui,sans-serif;max-width:40em;margin:12vh auto;padding:0 1em;color:#223}code{background:#eee;padding:.1em .4em;border-radius:4px}</style>\n"
                  "</head>\n<body>\n<h1>It works</h1>\n<p>This page is served from the <code>www</code> folder on your PSPi.</p>\n"
                  "<p>Put your own pages there, over FTP or in Files. PHP files (<code>.php</code>) run too.</p>\n</body>\n</html>\n",
                  f);
            fclose(f);
        }
    }
}

int web_start(const char *home, int port, char *err, size_t cap)
{
    paths(home);
    if (alive(&W.nginx)) return 0;
    if (!web_installed()) {
        snprintf(err, cap, "nginx isn't installed.");
        return -1;
    }
    prepare_dirs(home);
    {
        char pf[540];
        snprintf(pf, sizeof pf, "%s/nginx.pid", W.run);
        stale_stop(pf, "nginx");
        snprintf(pf, sizeof pf, "%s/php-fpm.pid", W.run);
        stale_stop(pf, "php-fpm");
    }
    if (access(W.conf, R_OK) != 0 && write_default(home, port) != 0) {
        snprintf(err, cap, "Couldn't write %s.", W.conf);
        return -1;
    }
    migrate(port);
    int lp = config_port();
    if (lp > 0 && lp < unprivileged_start() && geteuid() != 0) {
        snprintf(err, cap,
                 "Port %d needs a permission this system hasn't given yet. Run install.sh again to allow it, or set a port of 1024 or higher in the settings file.", lp);
        return -1;
    }
    char test[400];
    if (web_config_test(test, sizeof test) != 0) {
        snprintf(err, cap, "The settings file has a mistake: %s", test);
        return -1;
    }
    char ver[24];
    char sock[540];
    snprintf(sock, sizeof sock, "%s/php.sock", W.run);
    unlink(sock);
    if (web_php_available(ver, sizeof ver)) {
        char pconf[540], plog[540];
        snprintf(pconf, sizeof pconf, "%s/php-fpm.conf", W.run);
        snprintf(plog, sizeof plog, "%s/php-fpm.out", W.run);
        write_php_conf(pconf);
        char *pv[] = { W.php_bin, "-F", "-y", pconf, geteuid() == 0 ? "-R" : NULL, NULL };
        W.php = spawn(pv, plog);
    }
    char elog[540], nlog[540];
    snprintf(elog, sizeof elog, "%s/error.log", W.run);
    snprintf(nlog, sizeof nlog, "%s/nginx.out", W.run);
    char prefix[540];
    snprintf(prefix, sizeof prefix, "%s/", W.run);
    char *nv[] = { W.nginx_bin, "-c", W.conf, "-p", prefix, "-e", elog, NULL };
    W.nginx = spawn(nv, nlog);
    /* did it stay up? a port that's taken makes it stop at once */
    for (int i = 0; i < 12; i++) {
        usleep(100000);
        if (!alive(&W.nginx)) {
            char why[240];
            log_tail(nlog, why, sizeof why);
            if (!why[0]) log_tail(elog, why, sizeof why);
            snprintf(err, cap, "nginx stopped at once.%s%s", why[0] ? " " : "", why);
            if (W.php > 0) kill(W.php, SIGTERM), alive(&W.php);
            W.php = 0;
            return -1;
        }
    }
    return 0;
}

static void end(pid_t *p, int sig)
{
    if (*p <= 0) return;
    kill(*p, sig);
    for (int i = 0; i < 20; i++) {
        if (!alive(p)) return;
        usleep(100000);
    }
    kill(*p, SIGKILL);
    waitpid(*p, NULL, 0);
    *p = 0;
}

void web_stop(void)
{
    end(&W.nginx, SIGQUIT);
    end(&W.php, SIGTERM);
}

void web_status(WebStatus *st)
{
    memset(st, 0, sizeof *st);
    paths(home_of());
    st->running = alive(&W.nginx);
    st->php = alive(&W.php);
    st->port = config_port();
    snprintf(st->root, sizeof st->root, "%s", W.www);
    web_php_available(st->php_version, sizeof st->php_version);
    char p[540];
    snprintf(p, sizeof p, "%s/error.log", W.run);
    log_tail(p, st->last, sizeof st->last);
}

void web_reload(void)
{
    if (alive(&W.nginx)) kill(W.nginx, SIGHUP);
}

int web_config_test(char *out, size_t cap)
{
    paths(home_of());
    if (!web_installed()) {
        snprintf(out, cap, "nginx isn't installed.");
        return -1;
    }
    prepare_dirs(home_of());
    char cmd[2400], elog[540];
    snprintf(elog, sizeof elog, "%s/error.log", W.run);
    snprintf(cmd, sizeof cmd, "'%s' -t -c '%s' -p '%s/' -e '%s' 2>&1", W.nginx_bin, W.conf, W.run, elog);
    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(out, cap, "Couldn't run nginx.");
        return -1;
    }
    char all[1500] = "", line[400];
    while (fgets(line, sizeof line, p)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (strlen(all) + n + 2 < sizeof all) {
            if (all[0]) strcat(all, " ");
            strcat(all, line);
        }
    }
    int rc = pclose(p);
    /* the first lines say what's wrong; "test is successful" means it's fine */
    snprintf(out, cap, "%s", all);
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0 ? 0 : -1;
}
