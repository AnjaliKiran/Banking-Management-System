/*
 * roles.h - one menu loop per role.  Each *_session() returns
 * SESSION_LOGOUT (back to the main menu) or SESSION_EXIT (disconnect).
 */
#ifndef ROLES_H
#define ROLES_H

#define SESSION_LOGOUT 0
#define SESSION_EXIT   1

/* File descriptors of all tables, opened once per client process.
 * They are never closed while the client is connected: closing any fd of a
 * file drops ALL fcntl locks this process holds on that file. */
typedef struct {
    int customers, employees, admins, loans, feedback, transactions, sessions;
} Database;

extern Database db;

int customer_session(int sock, int id);
int employee_session(int sock, int id);
int manager_session(int sock, int id);
int admin_session(int sock, int id);

/* Shared operations (employee.c) */
void show_passbook(int sock, int customer_id);
void modify_customer(int sock);
void change_employee_password(int sock, int id);
int  customer_exists(int id);

#endif
