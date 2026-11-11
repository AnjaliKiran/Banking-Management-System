# Banking Management System (CS-513 Mini Project)

A client–server banking system in C for Linux. The server keeps all data in
`.txt` files and serves many clients at the same time over TCP sockets.

## Build & run

```bash
make                 # builds ./server and ./client
./server             # terminal 1 (default port 8080; ./server 9000 for another port)
./client             # terminal 2, 3, ... (./client <server-ip> <port>)
```

On first start, the server creates `data/` and a default administrator:
**ID 1, password `admin123`**.

| Command | What it does |
|---|---|
| `make test` | runs `tests/run_tests.sh`: concurrency, single-session and crash-recovery checks on a temporary copy (your `data/` is untouched) |
| `make zip` | creates `../BankingSystem.zip` for submission (sources, diagrams, report, tests, `data/` text files) |
| `make reset` | deletes all data |
| `make clean` | removes the binaries |

**Dependencies:** Linux, `gcc`, `make` (and `zip` for `make zip`). No external libraries.

## Diagrams and documentation

* `class_diagram.png`: class diagram of the implemented structs and modules
* `architecture_diagram.png`: clients, server process, forked children, data files
* `Banking_Management_System_Report.docx`: full project report (design, concurrency and ACID, demo transcripts, tests)

### Suggested demo order
1. Admin (ID 1) → add an Employee and a Manager.
2. Employee → add customers (each one gets an account number).
3. Customer → deposit / withdraw / transfer / apply for loan / feedback.
4. Manager → assign the loan to the employee, review feedback, deactivate an account.
5. Employee → process + approve the loan → the customer's balance is credited.

## Files

| File | Purpose |
|---|---|
| `server.c` | socket server, `fork()` per client, login, one-session-per-user, semaphore, reaping children + journal recovery |
| `client.c` | terminal client (hides password input with `termios`) |
| `db.c / db.h` | text-file database: fixed-size records, `fcntl` locking, write-ahead journal |
| `net.c / net.h` | send/receive helpers used by the server |
| `customer.c`, `employee.c`, `manager.c`, `admin.c` | one menu per role |
| `common.h` | port + protocol markers shared by client and server |
| `tests/run_tests.sh` | automated concurrency and crash-recovery demonstration |

## Data files (`data/`)

Each line is one record of exactly 256 bytes: `|`-separated fields padded with
spaces. The record id is its line number.

| File | Fields |
|---|---|
| `customers.txt` | id, first, last, password, balance, loan, status (ACTIVE/INACTIVE) |
| `employees.txt` | id, first, last, password, role (EMPLOYEE/MANAGER), status |
| `admins.txt` | id, first, last, password |
| `loans.txt` | id, customer, amount, status (PENDING→ASSIGNED→IN_REVIEW→APPROVED/REJECTED), employee |
| `feedback.txt` | id, customer, message, status (NEW/REVIEWED) |
| `transactions.txt` | id, customer, timestamp, type, amount, balance-after, other account |
| `sessions.lock` | stays empty; only used to hold locks |
| `journal/<pid>.log` | exists only while a money operation is being saved |

## Concurrency, synchronization & ACID

* **Processes:** `accept()` → `fork()`; the parent reaps children with `waitpid()`.
* **Record locking:** fixed-size records let `fcntl(F_SETLKW)` lock *one* record
  (`offset = (id-1)*256`). Two clients working on different accounts never wait for
  each other. Reads use `F_RDLCK`, updates use `F_WRLCK`.
* **Atomic id generation:** a new record is appended under a whole-file write
  lock, so two clients can never get the same id.
* **Transfers:** both account records are locked, always **lower id first**, which
  prevents deadlock. Both balances and both transaction log entries are written
  before either lock is released (isolation).
* **Write-ahead journal (atomicity):** deposits, withdrawals, transfers, loan approvals
  and opening deposits are saved with `txn_begin()` / `txn_commit()`. Commit first writes
  all new records plus a `COMMIT` mark to `data/journal/<pid>.log` and syncs it, then
  writes the tables, then deletes the journal. A crash before the mark leaves no trace;
  a crash after it is re-applied by the parent as soon as it reaps the child, or at the
  next server start if the whole server died.
* **Lock order** used everywhere: loan record → customer record → transactions file.
* **No lock is held while waiting for user input.** Input is collected first, then
  the code does lock → re-read → validate → write → unlock.
* **One session per user:** at login the child takes a non-blocking `F_SETLK` write
  lock on byte `<user key>` of `data/sessions.lock`. A second login for the same
  user fails. If a client crashes, its process exits and the kernel releases the lock.
* **Semaphore (System V IPC):** limits the server to `MAX_CLIENTS` concurrent clients.
  `SEM_UNDO` returns the slot even if a child is killed.
* **Durability:** the journal and every table write are synced with `fdatasync()` before
  the user sees a confirmation.
* **System calls used:** `socket/bind/listen/accept/connect`, `fork/waitpid`,
  `open/pread/pwrite/write/fstat/fdatasync/unlink/close`, `fcntl`, `semget/semop/semctl`,
  `sigaction/signal`, `mkdir`, `read`, `tcgetattr/tcsetattr`.

## Tests (`make test`)

1. 200 simultaneous opposite transfers between two accounts → total money unchanged,
   400 log entries, no deadlock.
2. A second login to an account that is already logged in is refused.
3. A server process killed **before** the commit point (`BANK_CRASH_POINT=before_commit`)
   → balance and passbook unchanged, journal discarded.
4. A server process killed **after** the commit point (`BANK_CRASH_POINT=after_commit`)
   → the deposit and its passbook entry are recovered from the journal.

`BANK_CRASH_POINT` is a test hook only; leave it unset in normal use.
