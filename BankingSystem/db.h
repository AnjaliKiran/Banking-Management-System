/*
 * db.h - text-file "database" layer.
 *
 * Every table is a .txt file made of FIXED-SIZE records (REC_SIZE bytes,
 * '|' separated fields, padded with spaces, ending in '\n').  Because each
 * record has a fixed size, record N lives at byte offset (N-1)*REC_SIZE.
 * That lets us:
 *   - read/update one record in place with pread()/pwrite()
 *   - lock exactly one record with fcntl() (record locking), so two clients
 *     touching different accounts never block each other.
 * The record id is its position in the file, so the next id is simply
 * (file size / REC_SIZE) + 1, computed while holding a whole-file lock.
 */
#ifndef DB_H
#define DB_H

#include <fcntl.h>

#define REC_SIZE 256

#define DATA_DIR         "data"
#define CUSTOMER_FILE    DATA_DIR "/customers.txt"
#define EMPLOYEE_FILE    DATA_DIR "/employees.txt"   /* employees + managers */
#define ADMIN_FILE       DATA_DIR "/admins.txt"
#define LOAN_FILE        DATA_DIR "/loans.txt"
#define FEEDBACK_FILE    DATA_DIR "/feedback.txt"
#define TRANSACTION_FILE DATA_DIR "/transactions.txt"
#define SESSION_FILE     DATA_DIR "/sessions.lock"   /* one-session-per-user locks */
#define JOURNAL_DIR      DATA_DIR "/journal"         /* write-ahead journals, one per process */

/* Field sizes (sscanf widths in db.c are these minus 1) */
#define NAME_LEN   32
#define PASS_LEN   32
#define STATUS_LEN 16
#define MSG_LEN    150
#define TIME_LEN   24

typedef struct {
    int id;
    char first[NAME_LEN];
    char last[NAME_LEN];
    char password[PASS_LEN];
    double balance;
    double loan;                 /* total approved loan amount */
    char status[STATUS_LEN];     /* ACTIVE / INACTIVE */
} Customer;

typedef struct {
    int id;
    char first[NAME_LEN];
    char last[NAME_LEN];
    char password[PASS_LEN];
    char role[STATUS_LEN];       /* EMPLOYEE / MANAGER */
    char status[STATUS_LEN];     /* ACTIVE / INACTIVE */
} Employee;

typedef struct {
    int id;
    char first[NAME_LEN];
    char last[NAME_LEN];
    char password[PASS_LEN];
} Admin;

typedef struct {
    int id;
    int customer_id;
    double amount;
    char status[STATUS_LEN];     /* PENDING / ASSIGNED / IN_REVIEW / APPROVED / REJECTED */
    int employee_id;             /* 0 until a manager assigns it */
} Loan;

typedef struct {
    int id;
    int customer_id;
    char message[MSG_LEN];
    char status[STATUS_LEN];     /* NEW / REVIEWED */
} Feedback;

typedef struct {
    int id;
    int customer_id;
    char timestamp[TIME_LEN];
    char type[STATUS_LEN];       /* DEPOSIT / WITHDRAW / TRANSFER_IN / TRANSFER_OUT / LOAN_CREDIT */
    double amount;
    double balance;              /* balance after this transaction */
    int other_id;                /* counterparty account for transfers, else 0 */
} Transaction;

/* Low level */
int  db_open(const char *path);
int  record_count(int fd);
void lock_record(int fd, int id, short type);   /* type: F_RDLCK / F_WRLCK */
void unlock_record(int fd, int id);
void lock_file(int fd, short type);
void unlock_file(int fd);

/* get_* return 1 on success, 0 if the id does not exist.
 * put_* overwrite an existing record (caller holds the record's write lock).
 * add_* assign the next id under a whole-file write lock; return the id or -1. */
int get_customer(int fd, int id, Customer *c);
int put_customer(int fd, const Customer *c);
int add_customer(int fd, Customer *c);

int get_employee(int fd, int id, Employee *e);
int put_employee(int fd, const Employee *e);
int add_employee(int fd, Employee *e);

int get_admin(int fd, int id, Admin *a);
int put_admin(int fd, const Admin *a);
int add_admin(int fd, Admin *a);

int get_loan(int fd, int id, Loan *l);
int put_loan(int fd, const Loan *l);
int add_loan(int fd, Loan *l);

int get_feedback(int fd, int id, Feedback *f);
int put_feedback(int fd, const Feedback *f);
int add_feedback(int fd, Feedback *f);

int get_transaction(int fd, int id, Transaction *t);
int log_transaction(int fd, int customer_id, const char *type,
                    double amount, double balance, int other_id);

/* Atomic multi-record updates (write-ahead journal).
 *   txn_begin();  put_x(...); add_y(...); ...;  ok = txn_commit();
 * Between begin and commit, writes are only staged in memory.  Commit first
 * writes every new record image plus a COMMIT mark to
 * data/journal/<pid>.log and syncs it, then applies the records to the
 * tables, then deletes the journal.  A crash before the COMMIT mark leaves
 * the tables untouched; a crash after it is repaired by recover_journal().
 * Files locked by add_*() inside the transaction stay locked until commit. */
void txn_begin(void);
int  txn_commit(void);                      /* 1 = committed, 0 = nothing changed */
int  recover_journal(const char *path);     /* records re-applied, -1 if discarded, 0 if none */

#endif
