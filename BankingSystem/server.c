/*
 * server.c - concurrent banking server.
 *
 *   ./server [port]
 *
 * - One child process per client (fork).  Children are reaped automatically.
 * - Data lives in fixed-record .txt files under data/ (see db.h).
 * - Consistency: fcntl record locks (see db.c).
 * - One session per user: the logged-in child holds a non-blocking fcntl
 *   write lock on byte <key> of data/sessions.lock.  A second login for the
 *   same user fails to get that lock.  If the client crashes, the child
 *   exits and the kernel releases the lock automatically.
 * - A System V semaphore caps the number of simultaneous clients.  SEM_UNDO
 *   gives the slot back even if a child is killed.
 * - Crash recovery: money operations commit through a write-ahead journal
 *   (db.c).  The parent reaps every child with waitpid(); if a child died
 *   during a commit, its journal is re-applied or discarded at once.  Any
 *   journal left by a whole-server crash is handled at the next start.
 */
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "db.h"
#include "net.h"
#include "roles.h"

#define MAX_CLIENTS 50

enum { ROLE_CUSTOMER = 1, ROLE_EMPLOYEE, ROLE_MANAGER, ROLE_ADMIN };

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

Database db;
static int semid = -1;

static int open_database(void)
{
    db.customers = db_open(CUSTOMER_FILE);
    db.employees = db_open(EMPLOYEE_FILE);
    db.admins = db_open(ADMIN_FILE);
    db.loans = db_open(LOAN_FILE);
    db.feedback = db_open(FEEDBACK_FILE);
    db.transactions = db_open(TRANSACTION_FILE);
    db.sessions = db_open(SESSION_FILE);
    return db.customers >= 0 && db.employees >= 0 && db.admins >= 0 && db.loans >= 0 &&
           db.feedback >= 0 && db.transactions >= 0 && db.sessions >= 0;
}

/* Session lock offset: employees and managers share one id space. */
static off_t session_key(int role, int id)
{
    if (role == ROLE_CUSTOMER)
        return id;
    if (role == ROLE_ADMIN)
        return 2000000000L + id;
    return 1000000000L + id;
}

static int session_lock(int role, int id, short type)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = session_key(role, id);
    fl.l_len = 1;
    return fcntl(db.sessions, F_SETLK, &fl);   /* non-blocking */
}

/* Returns 1 and fills *name if id/password are valid for this role. */
static int check_credentials(int sock, int role, int id, const char *pass, char *name, size_t len)
{
    int ok = 0;

    if (role == ROLE_CUSTOMER) {
        Customer c;
        lock_record(db.customers, id, F_RDLCK);
        ok = get_customer(db.customers, id, &c) && strcmp(c.password, pass) == 0;
        unlock_record(db.customers, id);
        if (ok && strcmp(c.status, "ACTIVE") != 0) {
            send_msg(sock, "This account is deactivated. Please contact the bank.\n");
            return 0;
        }
        if (ok)
            snprintf(name, len, "%s %s", c.first, c.last);
    } else if (role == ROLE_EMPLOYEE || role == ROLE_MANAGER) {
        Employee e;
        const char *want = role == ROLE_MANAGER ? "MANAGER" : "EMPLOYEE";
        lock_record(db.employees, id, F_RDLCK);
        ok = get_employee(db.employees, id, &e) && strcmp(e.password, pass) == 0 &&
             strcmp(e.role, want) == 0;
        unlock_record(db.employees, id);
        if (ok && strcmp(e.status, "ACTIVE") != 0) {
            send_msg(sock, "This staff account is inactive.\n");
            return 0;
        }
        if (ok)
            snprintf(name, len, "%s %s", e.first, e.last);
    } else {
        Admin a;
        lock_record(db.admins, id, F_RDLCK);
        ok = get_admin(db.admins, id, &a) && strcmp(a.password, pass) == 0;
        unlock_record(db.admins, id);
        if (ok)
            snprintf(name, len, "%s %s", a.first, a.last);
    }
    if (!ok)
        send_msg(sock, "Invalid ID or password.\n");
    return ok;
}

/* Returns SESSION_EXIT if the user chose Exit inside their menu. */
static int login(int sock, int role)
{
    char pass[PASS_LEN], name[2 * NAME_LEN + 2];
    int id, result;

    id = ask_int(sock, "User ID: ");
    ask_secret(sock, "Password: ", pass, sizeof pass);
    if (id <= 0 || !check_credentials(sock, role, id, pass, name, sizeof name))
        return SESSION_LOGOUT;

    if (session_lock(role, id, F_WRLCK) < 0) {
        send_msg(sock, "This user is already logged in from another session.\n");
        return SESSION_LOGOUT;
    }
    printf("[pid %d] login role=%d id=%d\n", getpid(), role, id);
    send_msg(sock, "\nWelcome, %s!\n", name);

    switch (role) {
    case ROLE_CUSTOMER: result = customer_session(sock, id); break;
    case ROLE_EMPLOYEE: result = employee_session(sock, id); break;
    case ROLE_MANAGER:  result = manager_session(sock, id); break;
    default:            result = admin_session(sock, id); break;
    }

    session_lock(role, id, F_UNLCK);
    printf("[pid %d] logout role=%d id=%d\n", getpid(), role, id);
    if (result == SESSION_LOGOUT)
        send_msg(sock, "Logged out.\n");
    return result;
}

static void handle_client(int sock)
{
    struct sembuf take = { 0, -1, IPC_NOWAIT | SEM_UNDO };

    if (semop(semid, &take, 1) < 0) {
        send_msg(sock, "Server is busy, try again later.\n");
        return;
    }
    if (!open_database()) {
        send_msg(sock, "Server database error.\n");
        return;
    }

    for (;;) {
        int choice;
        send_msg(sock, "\n====== Banking Management System ======\n"
                       "1. Customer Login\n"
                       "2. Employee Login\n"
                       "3. Manager Login\n"
                       "4. Administrator Login\n"
                       "5. Exit\n");
        choice = ask_int(sock, "Choice: ");
        if (choice == 5)
            break;
        if (choice < 1 || choice > 4) {
            send_msg(sock, "Invalid choice.\n");
            continue;
        }
        if (login(sock, choice) == SESSION_EXIT)
            break;
    }
    send_msg(sock, "Goodbye!\n");
}

static void report_recovery(const char *who, int result)
{
    if (result > 0)
        printf("[recovery] %s: committed transaction re-applied (%d records)\n", who, result);
    else if (result < 0)
        printf("[recovery] %s: uncommitted transaction discarded, no changes made\n", who);
}

/* Journals left behind by a server crash. */
static void recover_all_journals(void)
{
    DIR *dir = opendir(JOURNAL_DIR);
    struct dirent *e;
    char path[300];

    if (!dir)
        return;
    while ((e = readdir(dir)) != NULL) {
        if (!strstr(e->d_name, ".log"))
            continue;
        snprintf(path, sizeof path, JOURNAL_DIR "/%s", e->d_name);
        report_recovery(e->d_name, recover_journal(path));
    }
    closedir(dir);
}

/* Reaps finished children; a child that died mid-commit left a journal. */
static void reap_children(void)
{
    char path[64], who[32];
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        snprintf(path, sizeof path, JOURNAL_DIR "/%d.log", (int)pid);
        snprintf(who, sizeof who, "child %d", (int)pid);
        report_recovery(who, recover_journal(path));
    }
}

static void on_child_exit(int sig)
{
    (void)sig;                  /* only interrupts accept(); reaping happens in main loop */
}

static void init_storage(void)
{
    Admin a;

    if (mkdir(DATA_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir " DATA_DIR);
        exit(1);
    }
    if (mkdir(JOURNAL_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir " JOURNAL_DIR);
        exit(1);
    }
    recover_all_journals();
    if (!open_database()) {
        perror("open database");
        exit(1);
    }
    if (record_count(db.admins) == 0) {
        memset(&a, 0, sizeof a);
        strcpy(a.first, "System");
        strcpy(a.last, "Admin");
        strcpy(a.password, "admin123");
        add_admin(db.admins, &a);
        printf("Created default administrator: ID %d, password admin123\n", a.id);
    }
    /* The parent must not keep these open: children open their own. */
    close(db.customers); close(db.employees); close(db.admins); close(db.loans);
    close(db.feedback); close(db.transactions); close(db.sessions);
}

static void on_shutdown(int sig)
{
    (void)sig;
    if (semid >= 0)
        semctl(semid, 0, IPC_RMID);
    _exit(0);
}

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    int listen_fd, opt = 1;
    struct sockaddr_in addr;
    struct sigaction sa;
    union semun arg;

    setvbuf(stdout, NULL, _IOLBF, 0);       /* one log line at a time, even into a file */
    init_storage();

    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) {
        perror("semget");
        return 1;
    }
    arg.val = MAX_CLIENTS;
    semctl(semid, 0, SETVAL, arg);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_child_exit;          /* no SA_RESTART: accept() returns EINTR */
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* a closed client must not kill us */
    signal(SIGINT, on_shutdown);
    signal(SIGTERM, on_shutdown);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        on_shutdown(0);
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 16) < 0) {
        perror("bind/listen");
        on_shutdown(0);
    }
    printf("Bank server listening on port %d (Ctrl+C to stop)\n", port);

    for (;;) {
        int client;
        pid_t pid;

        reap_children();
        client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            close(listen_fd);
            handle_client(client);
            close(client);
            exit(0);            /* releases all locks + semaphore slot */
        }
        if (pid < 0)
            perror("fork");
        close(client);
    }
}
