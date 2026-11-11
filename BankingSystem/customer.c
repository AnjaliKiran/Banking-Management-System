/*
 * customer.c - customer menu.
 *
 * Rule followed everywhere: collect all user input FIRST, then
 * lock -> re-read record -> validate -> write -> log -> unlock.
 * A lock is never held while waiting for a human to type.
 */
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "net.h"
#include "roles.h"

static int is_active(const Customer *c)
{
    return strcmp(c->status, "ACTIVE") == 0;
}

static void view_balance(int sock, int id)
{
    Customer c;
    int ok;

    lock_record(db.customers, id, F_RDLCK);
    ok = get_customer(db.customers, id, &c);
    unlock_record(db.customers, id);

    if (!ok) {
        send_msg(sock, "Could not read your account.\n");
        return;
    }
    send_msg(sock, "\nAccount No.      : %d\n"
                   "Name             : %s %s\n"
                   "Balance          : %.2f\n"
                   "Loan outstanding : %.2f\n"
                   "Status           : %s\n",
             c.id, c.first, c.last, c.balance, c.loan, c.status);
}

static void deposit(int sock, int id)
{
    Customer c;
    int ok;
    double amount = ask_amount(sock, "Amount to deposit: ");

    if (amount <= 0) {
        send_msg(sock, "Amount must be greater than 0.\n");
        return;
    }

    lock_record(db.customers, id, F_WRLCK);
    if (!get_customer(db.customers, id, &c) || !is_active(&c)) {
        unlock_record(db.customers, id);
        send_msg(sock, "Account is not active.\n");
        return;
    }
    c.balance += amount;
    txn_begin();
    put_customer(db.customers, &c);
    log_transaction(db.transactions, id, "DEPOSIT", amount, c.balance, 0);
    ok = txn_commit();
    unlock_record(db.customers, id);

    if (ok)
        send_msg(sock, "Deposited %.2f. New balance: %.2f\n", amount, c.balance);
    else
        send_msg(sock, "Deposit failed; no changes were made.\n");
}

static void withdraw(int sock, int id)
{
    Customer c;
    int ok;
    double amount = ask_amount(sock, "Amount to withdraw: ");

    if (amount <= 0) {
        send_msg(sock, "Amount must be greater than 0.\n");
        return;
    }

    lock_record(db.customers, id, F_WRLCK);
    if (!get_customer(db.customers, id, &c) || !is_active(&c)) {
        unlock_record(db.customers, id);
        send_msg(sock, "Account is not active.\n");
        return;
    }
    if (c.balance < amount) {
        unlock_record(db.customers, id);
        send_msg(sock, "Insufficient balance (available: %.2f).\n", c.balance);
        return;
    }
    c.balance -= amount;
    txn_begin();
    put_customer(db.customers, &c);
    log_transaction(db.transactions, id, "WITHDRAW", amount, c.balance, 0);
    ok = txn_commit();
    unlock_record(db.customers, id);

    if (ok)
        send_msg(sock, "Withdrew %.2f. New balance: %.2f\n", amount, c.balance);
    else
        send_msg(sock, "Withdrawal failed; no changes were made.\n");
}

static void transfer(int sock, int id)
{
    Customer from, to;
    int to_id, first, second;
    double amount;

    to_id = ask_int(sock, "Recipient account number: ");
    if (to_id == id) {
        send_msg(sock, "Cannot transfer to your own account.\n");
        return;
    }
    if (!customer_exists(to_id)) {
        send_msg(sock, "No such account.\n");
        return;
    }
    amount = ask_amount(sock, "Amount to transfer: ");
    if (amount <= 0) {
        send_msg(sock, "Amount must be greater than 0.\n");
        return;
    }

    /* Lock both records, always lower id first, so two opposite transfers
     * (A->B and B->A) can never deadlock. */
    first = id < to_id ? id : to_id;
    second = id < to_id ? to_id : id;
    lock_record(db.customers, first, F_WRLCK);
    lock_record(db.customers, second, F_WRLCK);

    if (!get_customer(db.customers, id, &from) || !get_customer(db.customers, to_id, &to)) {
        send_msg(sock, "Could not read accounts.\n");
    } else if (!is_active(&from)) {
        send_msg(sock, "Your account is not active.\n");
    } else if (!is_active(&to)) {
        send_msg(sock, "Recipient account is not active.\n");
    } else if (from.balance < amount) {
        send_msg(sock, "Insufficient balance (available: %.2f).\n", from.balance);
    } else {
        /* Both updates happen while both locks are held: nobody can ever
         * observe money that has left one account but not reached the other.
         * The journal makes the four writes all-or-nothing even on a crash. */
        from.balance -= amount;
        to.balance += amount;
        txn_begin();
        put_customer(db.customers, &from);
        put_customer(db.customers, &to);
        log_transaction(db.transactions, id, "TRANSFER_OUT", amount, from.balance, to_id);
        log_transaction(db.transactions, to_id, "TRANSFER_IN", amount, to.balance, id);
        if (txn_commit())
            send_msg(sock, "Transferred %.2f to account %d. New balance: %.2f\n",
                     amount, to_id, from.balance);
        else
            send_msg(sock, "Transfer failed; no changes were made.\n");
    }

    unlock_record(db.customers, second);
    unlock_record(db.customers, first);
}

static void apply_loan(int sock, int id)
{
    Loan l;
    double amount = ask_amount(sock, "Loan amount: ");

    if (amount <= 0) {
        send_msg(sock, "Amount must be greater than 0.\n");
        return;
    }
    memset(&l, 0, sizeof l);
    l.customer_id = id;
    l.amount = amount;
    strcpy(l.status, "PENDING");
    l.employee_id = 0;

    if (add_loan(db.loans, &l) < 0)
        send_msg(sock, "Could not submit loan application.\n");
    else
        send_msg(sock, "Loan application #%d for %.2f submitted. Status: PENDING\n",
                 l.id, l.amount);
}

static void change_password(int sock, int id)
{
    char old[PASS_LEN], new1[PASS_LEN], new2[PASS_LEN];
    Customer c;

    ask_secret(sock, "Current password: ", old, sizeof old);
    ask_secret(sock, "New password: ", new1, sizeof new1);
    ask_secret(sock, "Confirm new password: ", new2, sizeof new2);
    if (strcmp(new1, new2) != 0) {
        send_msg(sock, "Passwords do not match.\n");
        return;
    }

    lock_record(db.customers, id, F_WRLCK);
    if (get_customer(db.customers, id, &c) && strcmp(c.password, old) == 0) {
        strcpy(c.password, new1);
        put_customer(db.customers, &c);
        send_msg(sock, "Password changed.\n");
    } else {
        send_msg(sock, "Current password is wrong.\n");
    }
    unlock_record(db.customers, id);
}

static void add_feedback_msg(int sock, int id)
{
    Feedback f;

    memset(&f, 0, sizeof f);
    ask(sock, "Your feedback: ", f.message, sizeof f.message);
    f.customer_id = id;
    strcpy(f.status, "NEW");

    if (add_feedback(db.feedback, &f) < 0)
        send_msg(sock, "Could not save feedback.\n");
    else
        send_msg(sock, "Thank you! Feedback #%d recorded.\n", f.id);
}

int customer_session(int sock, int id)
{
    for (;;) {
        send_msg(sock, "\n------ Customer Menu ------\n"
                       "1. View Account Balance\n"
                       "2. Deposit Money\n"
                       "3. Withdraw Money\n"
                       "4. Transfer Funds\n"
                       "5. Apply for a Loan\n"
                       "6. Change Password\n"
                       "7. Add Feedback\n"
                       "8. View Transaction History\n"
                       "9. Logout\n"
                       "10. Exit\n");
        switch (ask_int(sock, "Choice: ")) {
        case 1: view_balance(sock, id); break;
        case 2: deposit(sock, id); break;
        case 3: withdraw(sock, id); break;
        case 4: transfer(sock, id); break;
        case 5: apply_loan(sock, id); break;
        case 6: change_password(sock, id); break;
        case 7: add_feedback_msg(sock, id); break;
        case 8: show_passbook(sock, id); break;
        case 9: return SESSION_LOGOUT;
        case 10: return SESSION_EXIT;
        default: send_msg(sock, "Invalid choice.\n");
        }
    }
}
