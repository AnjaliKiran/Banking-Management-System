/*
 * common.h - definitions shared by the server and the client.
 *
 * Protocol: the server drives the whole conversation and sends plain text.
 * When it needs input it ends its text with a marker byte:
 *   MARK_INPUT  -> client reads one line from the keyboard and sends it
 *   MARK_SECRET -> same, but the client turns off echo (passwords)
 * When the server closes the socket, the client exits.
 */
#ifndef COMMON_H
#define COMMON_H

#define DEFAULT_PORT 8080
#define MARK_INPUT  '\x01'
#define MARK_SECRET '\x02'

#endif
