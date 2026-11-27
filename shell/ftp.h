/* A small FTP server, for copying files to and from the device. */
#ifndef PIXI_FTP_H
#define PIXI_FTP_H

#include <stddef.h>

typedef struct {
    int running, port, clients, loggedin;
    unsigned long long sent, received;
    char user[64], last[160];
} FtpStatus;

/* Serve root on port. The login is the account running this program, with that account's password. */
int ftp_start(const char *root, int port, char *err, size_t errsz);
/* Show only these folders of the root (call before ftp_start). */
void ftp_set_visible(const char *const *names, int n);
void ftp_stop(void);
void ftp_status(FtpStatus *out);
/* The address to show: the first non-loopback IPv4 address, or "" when there's no network. */
void ftp_address(char *out, size_t cap);

#endif
