/*
 * net.h - server-side helpers for talking to one client socket.
 * If the client disconnects, these call exit(), which ends the child process
 * and makes the kernel release every lock it held.
 */
#ifndef NET_H
#define NET_H

void   send_msg(int sock, const char *fmt, ...);
void   ask(int sock, const char *prompt, char *buf, int size);         /* non-empty text */
void   ask_secret(int sock, const char *prompt, char *buf, int size);  /* no echo */
int    ask_int(int sock, const char *prompt);       /* -1 if not a non-negative integer */
double ask_amount(int sock, const char *prompt);    /* -1 if not a valid amount >= 0 */

#endif
