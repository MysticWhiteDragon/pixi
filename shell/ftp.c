/* A small FTP server, for copying files to and from the device.
 *
 * One thread listens, one thread serves each client. Everything is confined to a root folder.
 * The login is the account running the shell, checked against that account's real password with PAM. */
#define _GNU_SOURCE
#include "ftp.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    int running, listen_fd, port;
    int clients, loggedin;
    unsigned long long sent, received;
    char root[1024], user[64], last[160];
    pthread_t thread;
} F = { .lock = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1 };

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&F.lock);
    vsnprintf(F.last, sizeof F.last, fmt, ap);
    pthread_mutex_unlock(&F.lock);
    va_end(ap);
}

static void add_bytes(int sent, long n)
{
    pthread_mutex_lock(&F.lock);
    if (sent) F.sent += (unsigned long long)n;
    else F.received += (unsigned long long)n;
    pthread_mutex_unlock(&F.lock);
}

/* ---------- login --------------------------------------------------------- */

static int pam_talk(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) r[i].resp = strdup((const char *)data);
    *resp = r;
    return PAM_SUCCESS;
}

static int check_password(const char *user, const char *pw)
{
    struct pam_conv conv = { pam_talk, (void *)pw };
    pam_handle_t *h = NULL;
    if (pam_start("login", user, &conv, &h) != PAM_SUCCESS) return 0;
    int rc = pam_authenticate(h, 0);
    if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(h, 0);
    pam_end(h, rc);
    return rc == PAM_SUCCESS;
}

/* ---------- one client ---------------------------------------------------- */

typedef struct {
    int fd, pasv_fd, authed, tries, binary;
    char cwd[1024], rnfr[1024], user[64], ip[64];
    long long rest;
    struct sockaddr_in local;
} Client;

static int say(Client *c, const char *fmt, ...)
{
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    memcpy(buf + n, "\r\n", 2);
    n += 2;
    int off = 0;
    while (off < n) {
        ssize_t w = send(c->fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w <= 0) return -1;
        off += (int)w;
    }
    return 0;
}

/* Turn what the client typed into a path under the root. The result starts with "/" and never climbs out. */
static void normalize(const char *cwd, const char *arg, char *out, size_t cap)
{
    char tmp[2200];
    if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
    char *parts[128];
    int n = 0;
    for (char *save = NULL, *p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, ".") || !*p) continue;
        if (!strcmp(p, "..")) {
            if (n) n--;
            continue;
        }
        if (n < 128) parts[n++] = p;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(out + o, cap - o, "/%s", parts[i]);
    if (!o) snprintf(out, cap, "/");
}

/* Only these folders in the root are shown or reachable. With none set, everything is. */
static char allow[16][64];
static int nallow;

void ftp_set_visible(const char *const *names, int n)
{
    nallow = 0;
    for (int i = 0; i < n && i < 16; i++) snprintf(allow[nallow++], sizeof allow[0], "%s", names[i]);
}

static int allowed(const char *name)
{
    if (!nallow) return 1;
    for (int i = 0; i < nallow; i++)
        if (!strcmp(allow[i], name)) return 1;
    return 0;
}

/* is the first part of a virtual path one of the visible folders? */
static int virt_allowed(const char *virt)
{
    if (!nallow || !strcmp(virt, "/")) return 1;
    char first[256];
    const char *p = virt + 1;
    size_t l = strcspn(p, "/");
    if (l >= sizeof first) return 0;
    memcpy(first, p, l);
    first[l] = 0;
    return allowed(first);
}

/* The real path for a virtual one, or 0 if it would leave the root through a link. */
static int real_path(const char *virt, char *out, size_t cap, int must_exist)
{
    char full[2300];
    if (!virt_allowed(virt)) return 0;
    snprintf(full, sizeof full, "%s%s", F.root, strcmp(virt, "/") ? virt : "");
    char rp[PATH_MAX];
    if (must_exist) {
        if (!realpath(full, rp)) return 0;
    } else {
        char *slash = strrchr(full, '/');
        if (!slash) return 0;
        *slash = 0;
        if (!realpath(full[0] ? full : "/", rp)) return 0;
        *slash = '/';
        size_t rl = strlen(F.root);
        if (strncmp(rp, F.root, rl) != 0 || (rp[rl] && rp[rl] != '/')) return 0;
        snprintf(out, cap, "%s%s", rp, slash);
        return 1;
    }
    size_t rl = strlen(F.root);
    if (strncmp(rp, F.root, rl) != 0 || (rp[rl] && rp[rl] != '/')) return 0;
    snprintf(out, cap, "%s", rp);
    return 1;
}

static int data_open(Client *c)
{
    if (c->pasv_fd < 0) {
        say(c, "425 Use PASV first.");
        return -1;
    }
    struct pollfd p = { c->pasv_fd, POLLIN, 0 };
    if (poll(&p, 1, 10000) <= 0) {
        close(c->pasv_fd);
        c->pasv_fd = -1;
        say(c, "425 Nothing connected for the transfer.");
        return -1;
    }
    int d = accept(c->pasv_fd, NULL, NULL);
    close(c->pasv_fd);
    c->pasv_fd = -1;
    if (d < 0) {
        say(c, "425 Couldn't open the data connection.");
        return -1;
    }
    say(c, "150 Opening the data connection.");
    return d;
}

static int send_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static void list_dir(Client *c, const char *real, int names_only)
{
    DIR *d = opendir(real);
    if (!d) {
        say(c, "550 Couldn't read that folder.");
        return;
    }
    int fd = data_open(c);
    if (fd < 0) {
        closedir(d);
        return;
    }
    struct dirent *e;
    char line[1500];
    int ok = 1;
    while (ok && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(real, F.root) && !allowed(e->d_name)) continue;
        if (names_only) {
            snprintf(line, sizeof line, "%s\r\n", e->d_name);
        } else {
            char p[2400];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", real, e->d_name);
            if (stat(p, &st) != 0) continue;
            char perm[11] = "----------";
            if (S_ISDIR(st.st_mode)) perm[0] = 'd';
            static const char *rwx = "rwxrwxrwx";
            for (int i = 0; i < 9; i++)
                if (st.st_mode & (1u << (8 - i))) perm[1 + i] = rwx[i];
            char when[32];
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof when, "%b %e %H:%M", &tm);
            snprintf(line, sizeof line, "%s 1 pixi pixi %lld %s %s\r\n", perm, (long long)st.st_size, when, e->d_name);
        }
        ok = send_all(fd, line, strlen(line)) == 0;
    }
    closedir(d);
    close(fd);
    say(c, ok ? "226 Done." : "426 The transfer stopped.");
}

static void do_retr(Client *c, const char *real)
{
    int f = open(real, O_RDONLY);
    struct stat st;
    if (f < 0 || fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f >= 0) close(f);
        say(c, "550 Couldn't open that file.");
        return;
    }
    if (c->rest > 0) lseek(f, (off_t)c->rest, SEEK_SET);
    c->rest = 0;
    int fd = data_open(c);
    if (fd < 0) {
        close(f);
        return;
    }
    const char *base = strrchr(real, '/');
    note("Sending %s", base ? base + 1 : real);
    char buf[32768];
    ssize_t n;
    int ok = 1;
    while (ok && (n = read(f, buf, sizeof buf)) > 0) {
        ok = send_all(fd, buf, (size_t)n) == 0;
        if (ok) add_bytes(1, (long)n);
        if (!F.running) ok = 0;
    }
    close(f);
    close(fd);
    say(c, ok ? "226 Done." : "426 The transfer stopped.");
}

static void do_stor(Client *c, const char *real, int append)
{
    int f = open(real, O_WRONLY | O_CREAT | (append ? O_APPEND : c->rest > 0 ? 0 : O_TRUNC), 0644);
    if (f < 0) {
        say(c, "550 Couldn't write that file.");
        return;
    }
    if (c->rest > 0 && !append) lseek(f, (off_t)c->rest, SEEK_SET);
    c->rest = 0;
    int fd = data_open(c);
    if (fd < 0) {
        close(f);
        return;
    }
    const char *base = strrchr(real, '/');
    note("Receiving %s", base ? base + 1 : real);
    char buf[32768];
    int ok = 1;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 1000);
        if (!F.running) {
            ok = 0;
            break;
        }
        if (pr == 0) continue;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n == 0) break;
        if (n < 0) {
            ok = 0;
            break;
        }
        if (write(f, buf, (size_t)n) != n) {
            ok = 0;
            break;
        }
        add_bytes(0, (long)n);
    }
    close(f);
    close(fd);
    say(c, ok ? "226 Done." : "426 The transfer stopped.");
}

static int pasv_open(Client *c)
{
    if (c->pasv_fd >= 0) close(c->pasv_fd);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = c->local;
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 1) != 0) {
        close(s);
        return -1;
    }
    socklen_t l = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &l);
    c->pasv_fd = s;
    return ntohs(a.sin_port);
}

static void handle_line(Client *c, char *line, int *quit)
{
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;
    for (char *p = line; *p; p++) *p = (char)toupper((unsigned char)*p);
    const char *cmd = line;

    if (!strcmp(cmd, "USER")) {
        snprintf(c->user, sizeof c->user, "%s", arg);
        c->authed = 0;
        say(c, "331 Password, please.");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        if (!c->user[0]) {
            say(c, "503 Send USER first.");
            return;
        }
        if (strcmp(c->user, F.user) == 0 && check_password(F.user, arg)) {
            c->authed = 1;
            pthread_mutex_lock(&F.lock);
            F.loggedin++;
            pthread_mutex_unlock(&F.lock);
            note("%s signed in from %s", F.user, c->ip);
            say(c, "230 Welcome.");
        } else {
            sleep(1);
            note("A sign-in from %s was refused", c->ip);
            say(c, "530 Wrong name or password.");
            if (++c->tries >= 3) *quit = 1;
        }
        return;
    }
    if (!strcmp(cmd, "QUIT")) {
        say(c, "221 Goodbye.");
        *quit = 1;
        return;
    }
    if (!strcmp(cmd, "NOOP")) {
        say(c, "200 OK.");
        return;
    }
    if (!strcmp(cmd, "SYST")) {
        say(c, "215 UNIX Type: L8");
        return;
    }
    if (!strcmp(cmd, "FEAT")) {
        say(c, "211-Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n EPSV\r\n211 End");
        return;
    }
    if (!strcmp(cmd, "OPTS")) {
        say(c, "200 OK.");
        return;
    }
    if (!strcmp(cmd, "AUTH") || !strcmp(cmd, "PORT") || !strcmp(cmd, "EPRT")) {
        say(c, "502 Not supported. Use passive mode.");
        return;
    }
    if (!c->authed) {
        say(c, "530 Sign in first.");
        return;
    }

    char virt[1100], real[2400];
    if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        say(c, "257 \"%s\" is the current folder.", c->cwd);
    } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        struct stat st;
        if (real_path(virt, real, sizeof real, 1) && stat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(c->cwd, sizeof c->cwd, "%s", virt);
            say(c, "250 OK.");
        } else say(c, "550 No such folder.");
    } else if (!strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
        normalize(c->cwd, "..", virt, sizeof virt);
        snprintf(c->cwd, sizeof c->cwd, "%s", virt);
        say(c, "250 OK.");
    } else if (!strcmp(cmd, "TYPE")) {
        c->binary = toupper((unsigned char)arg[0]) == 'I';
        say(c, "200 OK.");
    } else if (!strcmp(cmd, "MODE") || !strcmp(cmd, "STRU")) {
        say(c, "200 OK.");
    } else if (!strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV")) {
        int port = pasv_open(c);
        if (port < 0) {
            say(c, "425 Couldn't open a data port.");
            return;
        }
        if (cmd[0] == 'E') {
            say(c, "229 Entering extended passive mode (|||%d|).", port);
        } else {
            unsigned ip = ntohl(c->local.sin_addr.s_addr);
            say(c, "227 Entering passive mode (%u,%u,%u,%u,%d,%d).", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port >> 8, port & 255);
        }
    } else if (!strcmp(cmd, "LIST") || !strcmp(cmd, "NLST")) {
        const char *target = arg;
        while (*target == '-') { /* ignore options like -a */
            while (*target && *target != ' ') target++;
            while (*target == ' ') target++;
        }
        normalize(c->cwd, *target ? target : ".", virt, sizeof virt);
        struct stat st;
        if (!real_path(virt, real, sizeof real, 1) || stat(real, &st) != 0) {
            say(c, "550 No such folder.");
            return;
        }
        if (S_ISDIR(st.st_mode)) list_dir(c, real, cmd[0] == 'N');
        else say(c, "550 That isn't a folder.");
    } else if (!strcmp(cmd, "RETR")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (!real_path(virt, real, sizeof real, 1)) say(c, "550 No such file.");
        else do_retr(c, real);
    } else if (!strcmp(cmd, "STOR") || !strcmp(cmd, "APPE")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (!real_path(virt, real, sizeof real, 0)) say(c, "550 Not allowed.");
        else do_stor(c, real, cmd[0] == 'A');
    } else if (!strcmp(cmd, "REST")) {
        c->rest = atoll(arg);
        say(c, "350 Ready to restart.");
    } else if (!strcmp(cmd, "SIZE")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        struct stat st;
        if (real_path(virt, real, sizeof real, 1) && stat(real, &st) == 0 && S_ISREG(st.st_mode)) say(c, "213 %lld", (long long)st.st_size);
        else say(c, "550 No such file.");
    } else if (!strcmp(cmd, "MDTM")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        struct stat st;
        if (real_path(virt, real, sizeof real, 1) && stat(real, &st) == 0) {
            struct tm tm;
            char t[32];
            gmtime_r(&st.st_mtime, &tm);
            strftime(t, sizeof t, "%Y%m%d%H%M%S", &tm);
            say(c, "213 %s", t);
        } else say(c, "550 No such file.");
    } else if (!strcmp(cmd, "DELE")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (real_path(virt, real, sizeof real, 1) && unlink(real) == 0) {
            note("Deleted %s", virt);
            say(c, "250 Deleted.");
        } else say(c, "550 Couldn't delete that.");
    } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (real_path(virt, real, sizeof real, 0) && mkdir(real, 0755) == 0) say(c, "257 \"%s\" created.", virt);
        else say(c, "550 Couldn't create that folder.");
    } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (real_path(virt, real, sizeof real, 1) && rmdir(real) == 0) say(c, "250 Removed.");
        else say(c, "550 Couldn't remove that folder. Is it empty?");
    } else if (!strcmp(cmd, "RNFR")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (real_path(virt, real, sizeof real, 1)) {
            snprintf(c->rnfr, sizeof c->rnfr, "%s", real);
            say(c, "350 Send the new name.");
        } else say(c, "550 No such file.");
    } else if (!strcmp(cmd, "RNTO")) {
        normalize(c->cwd, arg, virt, sizeof virt);
        if (c->rnfr[0] && real_path(virt, real, sizeof real, 0) && rename(c->rnfr, real) == 0) say(c, "250 Renamed.");
        else say(c, "550 Couldn't rename that.");
        c->rnfr[0] = 0;
    } else {
        say(c, "502 Not supported.");
    }
}

static void *client_thread(void *arg)
{
    Client *c = arg;
    char buf[2048];
    size_t have = 0;
    int quit = 0;
    say(c, "220 Pixi is ready.");
    while (!quit && F.running) {
        struct pollfd p = { c->fd, POLLIN, 0 };
        int pr = poll(&p, 1, 500);
        if (pr == 0) continue;
        if (pr < 0) break;
        ssize_t n = recv(c->fd, buf + have, sizeof buf - have - 1, 0);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', have))) {
            *nl = 0;
            if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
            char line[2048];
            snprintf(line, sizeof line, "%s", buf);
            have -= (size_t)(nl - buf) + 1;
            memmove(buf, nl + 1, have);
            handle_line(c, line, &quit);
            if (quit) break;
        }
        if (have >= sizeof buf - 1) break; /* a line that long isn't FTP */
    }
    if (c->pasv_fd >= 0) close(c->pasv_fd);
    close(c->fd);
    pthread_mutex_lock(&F.lock);
    F.clients--;
    if (c->authed && F.loggedin > 0) F.loggedin--;
    pthread_mutex_unlock(&F.lock);
    free(c);
    return NULL;
}

static void *accept_thread(void *arg)
{
    (void)arg;
    while (F.running) {
        struct pollfd p = { F.listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(F.listen_fd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        pthread_mutex_lock(&F.lock);
        int full = F.clients >= 8;
        if (!full) F.clients++;
        pthread_mutex_unlock(&F.lock);
        if (full) {
            close(fd);
            continue;
        }
        Client *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->pasv_fd = -1;
        snprintf(c->cwd, sizeof c->cwd, "/");
        inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof c->ip);
        socklen_t ll = sizeof c->local;
        getsockname(fd, (struct sockaddr *)&c->local, &ll);
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, c) != 0) {
            pthread_mutex_lock(&F.lock);
            F.clients--;
            pthread_mutex_unlock(&F.lock);
            close(fd);
            free(c);
        } else pthread_detach(t);
    }
    return NULL;
}

int ftp_start(const char *root, int port, char *err, size_t errsz)
{
    if (F.running) return 0;
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        snprintf(err, errsz, "Couldn't find the account name.");
        return -1;
    }
    char rp[PATH_MAX];
    if (!realpath(root, rp)) {
        snprintf(err, errsz, "The folder to share isn't there.");
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        snprintf(err, errsz, "%s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        snprintf(err, errsz, "Couldn't use port %d: %s", port, strerror(errno));
        close(s);
        return -1;
    }
    pthread_mutex_lock(&F.lock);
    snprintf(F.root, sizeof F.root, "%s", rp);
    snprintf(F.user, sizeof F.user, "%s", pw->pw_name);
    F.last[0] = 0;
    F.sent = F.received = 0;
    F.clients = F.loggedin = 0;
    F.port = port;
    F.listen_fd = s;
    F.running = 1;
    pthread_mutex_unlock(&F.lock);
    if (pthread_create(&F.thread, NULL, accept_thread, NULL) != 0) {
        F.running = 0;
        close(s);
        snprintf(err, errsz, "Couldn't start the server.");
        return -1;
    }
    return 0;
}

void ftp_stop(void)
{
    if (!F.running) return;
    F.running = 0;
    pthread_join(F.thread, NULL);
    close(F.listen_fd);
    F.listen_fd = -1;
    for (int i = 0; i < 30 && F.clients > 0; i++) usleep(100000); /* clients notice within half a second */
}

void ftp_status(FtpStatus *o)
{
    memset(o, 0, sizeof *o);
    pthread_mutex_lock(&F.lock);
    o->running = F.running;
    o->port = F.port;
    o->clients = F.clients;
    o->loggedin = F.loggedin;
    o->sent = F.sent;
    o->received = F.received;
    snprintf(o->user, sizeof o->user, "%s", F.user);
    snprintf(o->last, sizeof o->last, "%s", F.last);
    pthread_mutex_unlock(&F.lock);
}

void ftp_address(char *out, size_t cap)
{
    out[0] = 0;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, out, (socklen_t)cap);
        if (!strncmp(p->ifa_name, "wlan", 4)) break; /* Wi-Fi first */
    }
    freeifaddrs(ifa);
}
