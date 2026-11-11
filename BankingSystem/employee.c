/*
 * employee.c - bank employee menu, plus operations shared with other roles
 * (passbook, modify customer, employee password change).
 */
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "net.h"
#include "roles.h"

int customer_exists(int id)
{
    return id >= 1 && id <= record_count(db.customers);
}

/* Passbook: every transaction of one customer.  A whole-file read lock
 * gives a consistent snapshot (no half-appended records). */
void show_passbook(int sock, int customer_id)
{
    Transaction t;
    int n, found = 0;

    lock_file(db.transactions, F_RDLCK);
    n = record_count(db.transactions);
    send_msg(sock, "\nPassbook for account %d\n"
                   "%-5s %-19s  %-12s %12s %12s %8s\n",
             customer_id, "TxnID", "Date/Time", "Type", "Amount", "Balance", "Other");
    for (int i = 1; i <= n; i++) {
        if (!get_transaction(db.transactions, i, &t) || t.customer_id != customer_id)
            continue;
        found = 1;
        if (t.other_id)
            send_msg(sock, "%-5d %-19s  %-12s %12.2f %12.2f %8d\n",
                     t.id, t.timestamp, t.type, t.amount, t.balance, t.other_id);
        else
            send_msg(sock, "%-5d %-19s  %-12s %12.2f %12.2f %8s\n",
                     t.id, t.timestamp, t.type, t.amount, t.balance, "-");
    }
    unlock_file(db.transactions);
    if (!found)
        send_msg(sock, "(no transactions)\n");
}

void modify_customer(int sock)
{
    Customer c;
    char value[NAME_LEN];
    int id, field;

    id = ask_int(sock, "Customer account number: ");
    if (!customer_exists(id)) {
        send_msg(sock, "No such customer.\n");
        return;
    }
    lock_record(db.customers, id, F_RDLCK);
    get_customer(db.customers, id, &c);
    unlock_record(db.customers, id);

    send_msg(sock, "Current: %s %s (status %s)\n"
                   "1. First name\n2. Last name\n3. Reset password\n4. Cancel\n",
             c.first, c.last, c.status);
    field = ask_int(sock, "Field to modify: ");
    if (field < 1 || field > 3)
        return;
    if (field == 3)
        ask_secret(sock, "New password: ", value, PASS_LEN);
    else
        ask(sock, "New value: ", value, NAME_LEN);

    lock_record(db.customers, id, F_WRLCK);
    get_customer(db.customers, id, &c);              /* re-read under the lock */
    if (field == 1)
        strcpy(c.first, value);
    else if (field == 2)
        strcpy(c.last, value);
    else
        strcpy(c.password, value);
    put_customer(db.customers, &c);
    unlock_record(db.customers, id);
    send_msg(sock, "Customer %d updated.\n", id);
}

void change_employee_password(int sock, int id)
{
    char old[PASS_LEN], new1[PASS_LEN], new2[PASS_LEN];
    Employee e;

    ask_secret(sock, "Current password: ", old, sizeof old);
    ask_secret(sock, "New password: ", new1, sizeof new1);
    ask_secret(sock, "Confirm new password: ", new2, sizeof new2);
    if (strcmp(new1, new2) != 0) {
        send_msg(sock, "Passwords do not match.\n");
        return;
    }

    lock_record(db.employees, id, F_WRLCK);
    if (get_employee(db.employees, id, &e) && strcmp(e.password, old) == 0) {
        strcpy(e.password, new1);
        put_employee(db.employees, &e);
        send_msg(sock, "Password changed.\n");
    } else {
        send_msg(sock, "Current password is wrong.\n");
    }
    unlock_record(db.employees, id);
}

static void add_new_customer(int sock)
{
    Customer c;
    double initial;

    memset(&c, 0, sizeof c);
    ask(sock, "First name: ", c.first, sizeof c.first);
    ask(sock, "Last name: ", c.last, sizeof c.last);
    ask_secret(sock, "Password: ", c.password, sizeof c.password);
    initial = ask_amount(sock, "Initial deposit (0 for none): ");
    if (initial < 0)
        return;
    c.balance = initial;
    c.loan = 0;
    strcpy(c.status, "ACTIVE");

    /* Account and opening deposit are saved together or not at all. */
    txn_begin();
    add_customer(db.customers, &c);
    if (initial > 0)
        log_transaction(db.transactions, c.id, "DEPOSIT", initial, initial, 0);
    if (!txn_commit()) {
        send_msg(sock, "Could not create customer.\n");
        return;
    }
    send_msg(sock, "Customer created. Account number: %d\n", c.id);
}

static int is_open_loan(const Loan *l)
{
    return strcmp(l->status, "ASSIGNED") == 0 || strcmp(l->status, "IN_REVIEW") == 0;
}

/* Lists this employee's loans; only_open restricts to undecided ones. */
static int list_my_loans(int sock, int emp_id, int only_open)
{
    Loan l;
    int n, found = 0;

    lock_file(db.loans, F_RDLCK);
    n = record_count(db.loans);
    send_msg(sock, "\n%-6s %-9s %12s  %-10s\n", "LoanID", "Customer", "Amount", "Status");
    for (int i = 1; i <= n; i++) {
        if (!get_loan(db.loans, i, &l) || l.employee_id != emp_id)
            continue;
        if (only_open && !is_open_loan(&l))
            continue;
        send_msg(sock, "%-6d %-9d %12.2f  %-10s\n", l.id, l.customer_id, l.amount, l.status);
        found++;
    }
    unlock_file(db.loans);
    if (!found)
        send_msg(sock, "(no loan applications)\n");
    return found;
}

/* Reads a loan id and checks it is an open loan assigned to this employee. */
static int pick_my_open_loan(int sock, int emp_id, Loan *l)
{
    int id = ask_int(sock, "Loan ID: ");
    int ok;

    lock_record(db.loans, id, F_RDLCK);
    ok = get_loan(db.loans, id, l);
    unlock_record(db.loans, id);
    if (!ok || l->employee_id != emp_id || !is_open_loan(l)) {
        send_msg(sock, "That is not an open loan assigned to you.\n");
        return 0;
    }
    return 1;
}

/* Process = review the application: show the applicant's account and
 * mark the loan IN_REVIEW. */
static void process_loan(int sock, int emp_id)
{
    Loan l;
    Customer c;

    if (!list_my_loans(sock, emp_id, 1) || !pick_my_open_loan(sock, emp_id, &l))
        return;

    lock_record(db.customers, l.customer_id, F_RDLCK);
    get_customer(db.customers, l.customer_id, &c);
    unlock_record(db.customers, l.customer_id);
    send_msg(sock, "\nLoan #%d: %.2f requested by account %d\n"
                   "Applicant: %s %s | Balance: %.2f | Existing loan: %.2f | Status: %s\n",
             l.id, l.amount, c.id, c.first, c.last, c.balance, c.loan, c.status);

    lock_record(db.loans, l.id, F_WRLCK);
    if (get_loan(db.loans, l.id, &l) && strcmp(l.status, "ASSIGNED") == 0) {
        strcpy(l.status, "IN_REVIEW");
        put_loan(db.loans, &l);
    }
    unlock_record(db.loans, l.id);
    send_msg(sock, "Loan marked IN_REVIEW. Use 'Approve/Reject Loans' to decide.\n");
}

static void approve_reject_loan(int sock, int emp_id)
{
    Loan l;
    Customer c;
    int decision, ok;

    if (!list_my_loans(sock, emp_id, 1) || !pick_my_open_loan(sock, emp_id, &l))
        return;
    decision = ask_int(sock, "1. Approve  2. Reject : ");
    if (decision != 1 && decision != 2) {
        send_msg(sock, "Cancelled.\n");
        return;
    }

    /* Lock order: loan record, then customer record (used consistently). */
    lock_record(db.loans, l.id, F_WRLCK);
    if (!get_loan(db.loans, l.id, &l) || l.employee_id != emp_id || !is_open_loan(&l)) {
        unlock_record(db.loans, l.id);
        send_msg(sock, "Loan was changed by someone else.\n");
        return;
    }
    if (decision == 1) {
        /* Loan status, customer balance and passbook entry: all or nothing. */
        lock_record(db.customers, l.customer_id, F_WRLCK);
        get_customer(db.customers, l.customer_id, &c);
        c.balance += l.amount;
        c.loan += l.amount;
        strcpy(l.status, "APPROVED");
        txn_begin();
        put_customer(db.customers, &c);
        log_transaction(db.transactions, c.id, "LOAN_CREDIT", l.amount, c.balance, 0);
        put_loan(db.loans, &l);
        ok = txn_commit();
        unlock_record(db.customers, l.customer_id);
    } else {
        strcpy(l.status, "REJECTED");
        ok = put_loan(db.loans, &l);
    }
    unlock_record(db.loans, l.id);

    if (ok)
        send_msg(sock, "Loan #%d %s.\n", l.id, l.status);
    else
        send_msg(sock, "Could not save the decision; no changes were made.\n");
}

static void view_customer_passbook(int sock)
{
    int id = ask_int(sock, "Customer account number: ");
    if (!customer_exists(id)) {
        send_msg(sock, "No such customer.\n");
        return;
    }
    show_passbook(sock, id);
}

int employee_session(int sock, int id)
{
    for (;;) {
        send_msg(sock, "\n------ Employee Menu ------\n"
                       "1. Add New Customer\n"
                       "2. Modify Customer Details\n"
                       "3. Process Loan Applications\n"
                       "4. Approve/Reject Loans\n"
                       "5. View Assigned Loan Applications\n"
                       "6. View Customer Transactions (Passbook)\n"
                       "7. Change Password\n"
                       "8. Logout\n"
                       "9. Exit\n");
        switch (ask_int(sock, "Choice: ")) {
        case 1: add_new_customer(sock); break;
        case 2: modify_customer(sock); break;
        case 3: process_loan(sock, id); break;
        case 4: approve_reject_loan(sock, id); break;
        case 5: list_my_loans(sock, id, 0); break;
        case 6: view_customer_passbook(sock); break;
        case 7: change_employee_password(sock, id); break;
        case 8: return SESSION_LOGOUT;
        case 9: return SESSION_EXIT;
        default: send_msg(sock, "Invalid choice.\n");
        }
    }
}
