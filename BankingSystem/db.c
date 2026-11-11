/*
 * db.c - fixed-size record storage on top of open/pread/pwrite/fcntl.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "db.h"

#define MAX_FDS      64
#define MAX_TXN_RECS 8

/* One staged record of an open transaction; also the journal's on-disk format. */
typedef struct {
    char path[64];                        /* table file, e.g. data/customers.txt */
    int id;
    char rec[REC_SIZE];
} JournalRec;

static const char COMMIT_MARK[8] = "COMMIT\n";

static const char *fd_path[MAX_FDS];      /* which table each fd belongs to */
static JournalRec pending[MAX_TXN_RECS];
static int pending_fd[MAX_TXN_RECS];
static int npending = -1;                 /* -1 = no transaction open */
static int held_fd[MAX_TXN_RECS];         /* files locked by add_*() during the transaction */
static int nheld;

int db_open(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd >= 0 && fd < MAX_FDS)
        fd_path[fd] = path;
    return fd;
}

static int find_pending(int fd, int id)
{
    for (int i = 0; i < npending; i++)
        if (pending_fd[i] == fd && pending[i].id == id)
            return i;
    return -1;
}

/* Counts staged (not yet written) appends too, so ids stay unique. */
int record_count(int fd)
{
    struct stat st;
    int n;

    if (fstat(fd, &st) < 0)
        return 0;
    n = (int)(st.st_size / REC_SIZE);
    for (int i = 0; i < npending; i++)
        if (pending_fd[i] == fd && pending[i].id > n)
            n = pending[i].id;
    return n;
}

/* ---------- locking (fcntl advisory record locks) ---------- */

static void set_lock(int fd, short type, off_t start, off_t len)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;                       /* 0 = up to end of file and beyond */
    while (fcntl(fd, F_SETLKW, &fl) < 0 && errno == EINTR)
        ;
}

void lock_record(int fd, int id, short type)
{
    set_lock(fd, type, (off_t)(id - 1) * REC_SIZE, REC_SIZE);
}

void unlock_record(int fd, int id)
{
    set_lock(fd, F_UNLCK, (off_t)(id - 1) * REC_SIZE, REC_SIZE);
}

void lock_file(int fd, short type)
{
    set_lock(fd, type, 0, 0);
}

void unlock_file(int fd)
{
    set_lock(fd, F_UNLCK, 0, 0);
}

/* ---------- raw record I/O ---------- */

static int read_rec(int fd, int id, char *buf)
{
    int p;

    if (id < 1 || id > record_count(fd))
        return 0;
    if ((p = find_pending(fd, id)) >= 0) {          /* read our own staged write */
        memcpy(buf, pending[p].rec, REC_SIZE);
    } else if (pread(fd, buf, REC_SIZE, (off_t)(id - 1) * REC_SIZE) != REC_SIZE) {
        return 0;
    }
    buf[REC_SIZE - 1] = '\0';
    return 1;
}

static int raw_write(int fd, int id, const char *buf)
{
    return pwrite(fd, buf, REC_SIZE, (off_t)(id - 1) * REC_SIZE) == REC_SIZE;
}

static int stage(int fd, int id, const char *buf)
{
    int p = find_pending(fd, id);

    if (p < 0) {
        if (npending == MAX_TXN_RECS || fd < 0 || fd >= MAX_FDS || !fd_path[fd])
            return 0;
        p = npending++;
        pending_fd[p] = fd;
        snprintf(pending[p].path, sizeof pending[p].path, "%s", fd_path[fd]);
        pending[p].id = id;
    }
    memcpy(pending[p].rec, buf, REC_SIZE);
    return 1;
}

static int write_rec(int fd, int id, const char *text)
{
    char buf[REC_SIZE];
    size_t len = strlen(text);

    if (len > REC_SIZE - 1)
        len = REC_SIZE - 1;
    memset(buf, ' ', REC_SIZE);
    memcpy(buf, text, len);
    buf[REC_SIZE - 1] = '\n';

    if (npending >= 0)                    /* inside a transaction: write at commit */
        return stage(fd, id, buf);
    if (!raw_write(fd, id, buf))
        return 0;
    fdatasync(fd);                        /* durability: data is on disk before we reply */
    return 1;
}

static void hold_file(int fd)
{
    for (int i = 0; i < nheld; i++)
        if (held_fd[i] == fd)
            return;
    if (nheld < MAX_TXN_RECS)
        held_fd[nheld++] = fd;
}

/* Appends a new record: the whole-file write lock makes "compute next id +
 * write" atomic, so two clients can never get the same id.  Inside a
 * transaction the lock is kept until txn_commit() has written the record. */
#define DEFINE_ADD(name, type)                         \
    int add_##name(int fd, type *x)                    \
    {                                                  \
        int ok;                                        \
        lock_file(fd, F_WRLCK);                        \
        x->id = record_count(fd) + 1;                  \
        ok = put_##name(fd, x);                        \
        if (npending >= 0)                             \
            hold_file(fd);                             \
        else                                           \
            unlock_file(fd);                           \
        return ok ? x->id : -1;                        \
    }

/* Every record ends with a trailing '|' so that fields may contain spaces. */

/* ---------- customers ---------- */

int get_customer(int fd, int id, Customer *c)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%31[^|]|%31[^|]|%31[^|]|%lf|%lf|%15[^|]|",
                  &c->id, c->first, c->last, c->password,
                  &c->balance, &c->loan, c->status) == 7;
}

int put_customer(int fd, const Customer *c)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%s|%s|%s|%.2f|%.2f|%s|",
             c->id, c->first, c->last, c->password,
             c->balance, c->loan, c->status);
    return write_rec(fd, c->id, buf);
}

DEFINE_ADD(customer, Customer)

/* ---------- employees / managers ---------- */

int get_employee(int fd, int id, Employee *e)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%31[^|]|%31[^|]|%31[^|]|%15[^|]|%15[^|]|",
                  &e->id, e->first, e->last, e->password,
                  e->role, e->status) == 6;
}

int put_employee(int fd, const Employee *e)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%s|%s|%s|%s|%s|",
             e->id, e->first, e->last, e->password, e->role, e->status);
    return write_rec(fd, e->id, buf);
}

DEFINE_ADD(employee, Employee)

/* ---------- admins ---------- */

int get_admin(int fd, int id, Admin *a)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%31[^|]|%31[^|]|%31[^|]|",
                  &a->id, a->first, a->last, a->password) == 4;
}

int put_admin(int fd, const Admin *a)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%s|%s|%s|", a->id, a->first, a->last, a->password);
    return write_rec(fd, a->id, buf);
}

DEFINE_ADD(admin, Admin)

/* ---------- loans ---------- */

int get_loan(int fd, int id, Loan *l)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%d|%lf|%15[^|]|%d|",
                  &l->id, &l->customer_id, &l->amount, l->status,
                  &l->employee_id) == 5;
}

int put_loan(int fd, const Loan *l)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%d|%.2f|%s|%d|",
             l->id, l->customer_id, l->amount, l->status, l->employee_id);
    return write_rec(fd, l->id, buf);
}

DEFINE_ADD(loan, Loan)

/* ---------- feedback ---------- */

int get_feedback(int fd, int id, Feedback *f)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%d|%149[^|]|%15[^|]|",
                  &f->id, &f->customer_id, f->message, f->status) == 4;
}

int put_feedback(int fd, const Feedback *f)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%d|%s|%s|",
             f->id, f->customer_id, f->message, f->status);
    return write_rec(fd, f->id, buf);
}

DEFINE_ADD(feedback, Feedback)

/* ---------- transactions (append-only) ---------- */

int get_transaction(int fd, int id, Transaction *t)
{
    char buf[REC_SIZE];
    if (!read_rec(fd, id, buf))
        return 0;
    return sscanf(buf, "%d|%d|%23[^|]|%15[^|]|%lf|%lf|%d|",
                  &t->id, &t->customer_id, t->timestamp, t->type,
                  &t->amount, &t->balance, &t->other_id) == 7;
}

static int put_transaction(int fd, const Transaction *t)
{
    char buf[REC_SIZE];
    snprintf(buf, sizeof buf, "%d|%d|%s|%s|%.2f|%.2f|%d|",
             t->id, t->customer_id, t->timestamp, t->type,
             t->amount, t->balance, t->other_id);
    return write_rec(fd, t->id, buf);
}

static DEFINE_ADD(transaction, Transaction)

int log_transaction(int fd, int customer_id, const char *type,
                    double amount, double balance, int other_id)
{
    Transaction t;
    time_t now = time(NULL);
    struct tm tm;

    memset(&t, 0, sizeof t);
    t.customer_id = customer_id;
    localtime_r(&now, &tm);
    strftime(t.timestamp, sizeof t.timestamp, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(t.type, sizeof t.type, "%s", type);
    t.amount = amount;
    t.balance = balance;
    t.other_id = other_id;
    return add_transaction(fd, &t);
}

/* ---------- write-ahead journal ---------- */

/* Test hook: BANK_CRASH_POINT=before_commit or after_commit makes the process
 * die at that point of every commit, to demonstrate recovery. */
static void crash_point(const char *where)
{
    const char *p = getenv("BANK_CRASH_POINT");
    if (p && strcmp(p, where) == 0)
        _exit(99);
}

static void journal_path(char *buf, size_t size, int pid)
{
    snprintf(buf, size, JOURNAL_DIR "/%d.log", pid);
}

void txn_begin(void)
{
    npending = 0;
    nheld = 0;
}

int txn_commit(void)
{
    char path[64];
    size_t len = (size_t)npending * sizeof(JournalRec);
    int jfd, ok;

    journal_path(path, sizeof path, (int)getpid());
    jfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ok = jfd >= 0 && write(jfd, pending, len) == (ssize_t)len;
    crash_point("before_commit");
    ok = ok && write(jfd, COMMIT_MARK, sizeof COMMIT_MARK) == (ssize_t)sizeof COMMIT_MARK
            && fdatasync(jfd) == 0;      /* the commit point */
    if (jfd >= 0)
        close(jfd);
    crash_point("after_commit");

    if (ok) {
        for (int i = 0; i < npending; i++)
            raw_write(pending_fd[i], pending[i].id, pending[i].rec);
        for (int i = 0; i < npending; i++)
            fdatasync(pending_fd[i]);
    }
    unlink(path);                         /* applied, or abandoned without changing anything */
    for (int i = 0; i < nheld; i++)
        unlock_file(held_fd[i]);
    npending = -1;
    nheld = 0;
    return ok;
}

int recover_journal(const char *path)
{
    struct stat st;
    JournalRec r;
    char mark[sizeof COMMIT_MARK];
    off_t body;
    int fd, committed, applied = 0;

    if ((fd = open(path, O_RDONLY)) < 0)
        return 0;
    fstat(fd, &st);
    body = st.st_size - (off_t)sizeof mark;
    committed = body >= 0 && body % (off_t)sizeof r == 0 &&
                pread(fd, mark, sizeof mark, body) == (ssize_t)sizeof mark &&
                memcmp(mark, COMMIT_MARK, sizeof mark) == 0;

    for (off_t off = 0; committed && off < body; off += sizeof r) {
        int tfd;
        if (pread(fd, &r, sizeof r, off) != (ssize_t)sizeof r)
            break;
        r.path[sizeof r.path - 1] = '\0';
        if ((tfd = open(r.path, O_RDWR)) < 0)
            continue;
        lock_record(tfd, r.id, F_WRLCK);
        raw_write(tfd, r.id, r.rec);      /* redo is safe to repeat: same bytes, same place */
        fdatasync(tfd);
        close(tfd);                       /* also releases the lock */
        applied++;
    }
    close(fd);
    unlink(path);
    return committed ? applied : -1;
}
