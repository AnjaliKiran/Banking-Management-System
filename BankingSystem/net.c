#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

#define MAX_AMOUNT 1e9

static void write_all(int sock, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(sock, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            exit(0);                      /* client is gone */
        }
        buf += n;
        len -= (size_t)n;
    }
}

void send_msg(int sock, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    if (len >= (int)sizeof buf)
        len = sizeof buf - 1;
    write_all(sock, buf, (size_t)len);
}

static void read_line(int sock, char *buf, int size)
{
    int len = 0;
    char ch;

    for (;;) {
        ssize_t n = read(sock, &ch, 1);
        if (n == 0)
            exit(0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            exit(0);
        }
        if (ch == '\n')
            break;
        if (len < size - 1)
            buf[len++] = ch;
    }
    buf[len] = '\0';
}

/* Trim spaces and replace characters that would break the record format. */
static void clean(char *s)
{
    char *start = s, *end;

    for (char *p = s; *p; p++)
        if (*p == '|' || (unsigned char)*p < 32)
            *p = ' ';
    while (*start == ' ')
        start++;
    memmove(s, start, strlen(start) + 1);
    end = s + strlen(s);
    while (end > s && end[-1] == ' ')
        *--end = '\0';
}

static void prompt_input(int sock, const char *prompt, char *buf, int size, char marker)
{
    for (;;) {
        send_msg(sock, "%s%c", prompt, marker);
        read_line(sock, buf, size);
        clean(buf);
        if (buf[0])
            return;
        send_msg(sock, "Input cannot be empty.\n");
    }
}

void ask(int sock, const char *prompt, char *buf, int size)
{
    prompt_input(sock, prompt, buf, size, MARK_INPUT);
}

void ask_secret(int sock, const char *prompt, char *buf, int size)
{
    prompt_input(sock, prompt, buf, size, MARK_SECRET);
}

int ask_int(int sock, const char *prompt)
{
    char buf[32], *end;
    long v;

    ask(sock, prompt, buf, sizeof buf);
    errno = 0;
    v = strtol(buf, &end, 10);
    if (*end != '\0' || errno || v < 0 || v > 1000000000L)
        return -1;
    return (int)v;
}

double ask_amount(int sock, const char *prompt)
{
    char buf[32], *end;
    double v;

    ask(sock, prompt, buf, sizeof buf);
    v = strtod(buf, &end);
    if (*end != '\0' || !isfinite(v) || v < 0 || v > MAX_AMOUNT) {
        send_msg(sock, "Invalid amount.\n");
        return -1;
    }
    return round(v * 100) / 100;          /* keep two decimal places */
}
