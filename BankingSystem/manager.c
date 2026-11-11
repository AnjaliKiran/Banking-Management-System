/*
 * manager.c - manager menu.
 */
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "net.h"
#include "roles.h"

static void set_customer_status(int sock)
{
    Customer c;
    int id, choice;

    id = ask_int(sock, "Customer account number: ");
    if (!customer_exists(id)) {
        send_msg(sock, "No such customer.\n");
        return;
    }
    lock_record(db.customers, id, F_RDLCK);
    get_customer(db.customers, id, &c);
    unlock_record(db.customers, id);
    send_msg(sock, "Account %d (%s %s) is currently %s.\n", c.id, c.first, c.last, c.status);

    choice = ask_int(sock, "1. Activate  2. Deactivate : ");
    if (choice != 1 && choice != 2) {
        send_msg(sock, "Cancelled.\n");
        return;
    }

    lock_record(db.customers, id, F_WRLCK);
    get_customer(db.customers, id, &c);
    strcpy(c.status, choice == 1 ? "ACTIVE" : "INACTIVE");
    put_customer(db.customers, &c);
    unlock_record(db.customers, id);
    send_msg(sock, "Account %d is now %s.\n", id, c.status);
}

static int list_pending_loans(int sock)
{
    Loan l;
    int n, found = 0;

    lock_file(db.loans, F_RDLCK);
    n = record_count(db.loans);
    send_msg(sock, "\nPending loan applications:\n%-6s %-9s %12s\n", "LoanID", "Customer", "Amount");
    for (int i = 1; i <= n; i++) {
        if (get_loan(db.loans, i, &l) && strcmp(l.status, "PENDING") == 0) {
            send_msg(sock, "%-6d %-9d %12.2f\n", l.id, l.customer_id, l.amount);
            found++;
        }
    }
    unlock_file(db.loans);
    if (!found)
        send_msg(sock, "(none)\n");
    return found;
}

static int list_active_employees(int sock)
{
    Employee e;
    int n, found = 0;

    lock_file(db.employees, F_RDLCK);
    n = record_count(db.employees);
    send_msg(sock, "\nEmployees:\n%-5s %s\n", "ID", "Name");
    for (int i = 1; i <= n; i++) {
        if (get_employee(db.employees, i, &e) && strcmp(e.role, "EMPLOYEE") == 0 &&
            strcmp(e.status, "ACTIVE") == 0) {
            send_msg(sock, "%-5d %s %s\n", e.id, e.first, e.last);
            found++;
        }
    }
    unlock_file(db.employees);
    if (!found)
        send_msg(sock, "(no active employees)\n");
    return found;
}

static void assign_loan(int sock)
{
    Loan l;
    Employee e;
    int loan_id, emp_id, ok;

    if (!list_pending_loans(sock))
        return;
    loan_id = ask_int(sock, "Loan ID to assign: ");
    if (!list_active_employees(sock))
        return;
    emp_id = ask_int(sock, "Employee ID: ");

    lock_record(db.employees, emp_id, F_RDLCK);
    ok = get_employee(db.employees, emp_id, &e) && strcmp(e.role, "EMPLOYEE") == 0 &&
         strcmp(e.status, "ACTIVE") == 0;
    unlock_record(db.employees, emp_id);
    if (!ok) {
        send_msg(sock, "Not an active employee.\n");
        return;
    }

    lock_record(db.loans, loan_id, F_WRLCK);
    if (!get_loan(db.loans, loan_id, &l) || strcmp(l.status, "PENDING") != 0) {
        send_msg(sock, "Loan is not pending (maybe already assigned).\n");
    } else {
        l.employee_id = emp_id;
        strcpy(l.status, "ASSIGNED");
        put_loan(db.loans, &l);
        send_msg(sock, "Loan #%d assigned to employee %d.\n", loan_id, emp_id);
    }
    unlock_record(db.loans, loan_id);
}

static void review_feedback(int sock)
{
    Feedback f;
    int n, found = 0, id;

    lock_file(db.feedback, F_RDLCK);
    n = record_count(db.feedback);
    send_msg(sock, "\n%-4s %-9s %-9s %s\n", "ID", "Customer", "Status", "Message");
    for (int i = 1; i <= n; i++) {
        if (get_feedback(db.feedback, i, &f)) {
            send_msg(sock, "%-4d %-9d %-9s %s\n", f.id, f.customer_id, f.status, f.message);
            found++;
        }
    }
    unlock_file(db.feedback);
    if (!found) {
        send_msg(sock, "(no feedback)\n");
        return;
    }

    id = ask_int(sock, "Feedback ID to mark REVIEWED (0 to skip): ");
    if (id <= 0)
        return;
    lock_record(db.feedback, id, F_WRLCK);
    if (get_feedback(db.feedback, id, &f)) {
        strcpy(f.status, "REVIEWED");
        put_feedback(db.feedback, &f);
        send_msg(sock, "Feedback #%d marked REVIEWED.\n", id);
    } else {
        send_msg(sock, "No such feedback.\n");
    }
    unlock_record(db.feedback, id);
}

int manager_session(int sock, int id)
{
    for (;;) {
        send_msg(sock, "\n------ Manager Menu ------\n"
                       "1. Activate/Deactivate Customer Account\n"
                       "2. Assign Loan Application to Employee\n"
                       "3. Review Customer Feedback\n"
                       "4. Change Password\n"
                       "5. Logout\n"
                       "6. Exit\n");
        switch (ask_int(sock, "Choice: ")) {
        case 1: set_customer_status(sock); break;
        case 2: assign_loan(sock); break;
        case 3: review_feedback(sock); break;
        case 4: change_employee_password(sock, id); break;
        case 5: return SESSION_LOGOUT;
        case 6: return SESSION_EXIT;
        default: send_msg(sock, "Invalid choice.\n");
        }
    }
}
