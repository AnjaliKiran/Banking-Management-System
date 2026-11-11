/*
 * client.c - thin terminal client.
 *
 *   ./client [server-ip] [port]
 *
 * Prints whatever the server sends.  When the server sends MARK_INPUT or
 * MARK_SECRET, reads one line from the keyboard (echo off for secrets) and
 * sends it back.  Exits when the server closes the connection.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            exit(1);
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* Reads one line from stdin and sends it (with '\n') to the server. */
static void send_input(int sock, int secret)
{
    char line[256];
    size_t len = 0;
    char ch;
    struct termios old, quiet;
    int restore = 0;

    if (secret && tcgetattr(STDIN_FILENO, &old) == 0) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
        restore = 1;
    }
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {               /* EOF on keyboard: leave */
            if (restore)
                tcsetattr(STDIN_FILENO, TCSANOW, &old);
            write_all(STDOUT_FILENO, "\n", 1);
            exit(0);
        }
        if (ch == '\n')
            break;
        if (len < sizeof line - 2)
            line[len++] = ch;
    }
    if (restore) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        write_all(STDOUT_FILENO, "\n", 1);
    }
    line[len++] = '\n';
    write_all(sock, line, len);
}

int main(int argc, char *argv[])
{
    const char *ip = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
    struct sockaddr_in addr;
    char buf[4096];
    ssize_t n;
    int sock;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server address: %s\n", ip);
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect (is the server running?)");
        return 1;
    }

    while ((n = read(sock, buf, sizeof buf)) > 0) {
        ssize_t start = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == MARK_INPUT || buf[i] == MARK_SECRET) {
                write_all(STDOUT_FILENO, buf + start, (size_t)(i - start));
                send_input(sock, buf[i] == MARK_SECRET);
                start = i + 1;
            }
        }
        write_all(STDOUT_FILENO, buf + start, (size_t)(n - start));
    }
    close(sock);
    return 0;
}
