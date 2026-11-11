/*
 * admin.c - administrator menu.
 */
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "net.h"
#include "roles.h"

static void add_new_employee(int sock)
{
    Employee e;
    int role;

    memset(&e, 0, sizeof e);
    ask(sock, "First name: ", e.first, sizeof e.first);
    ask(sock, "Last name: ", e.last, sizeof e.last);
    ask_secret(sock, "Password: ", e.password, sizeof e.password);
    role = ask_int(sock, "Role: 1. Employee  2. Manager : ");
    if (role != 1 && role != 2) {
        send_msg(sock, "Invalid role.\n");
        return;
    }
    strcpy(e.role, role == 1 ? "EMPLOYEE" : "MANAGER");
    strcpy(e.status, "ACTIVE");

    if (add_employee(db.employees, &e) < 0)
        send_msg(sock, "Could not add employee.\n");
    else
        send_msg(sock, "%s added. ID: %d\n", e.role, e.id);
}

static void list_employees(int sock)
{
    Employee e;
    int n;

    lock_file(db.employees, F_RDLCK);
    n = record_count(db.employees);
    send_msg(sock, "\n%-5s %-25s %-9s %s\n", "ID", "Name", "Role", "Status");
    for (int i = 1; i <= n; i++)
        if (get_employee(db.employees, i, &e))
            send_msg(sock, "%-5d %-12s %-12s %-9s %s\n", e.id, e.first, e.last, e.role, e.status);
    unlock_file(db.employees);
    if (n == 0)
        send_msg(sock, "(no employees)\n");
}

static int read_employee(int id, Employee *e)
{
    int ok;
    lock_record(db.employees, id, F_RDLCK);
    ok = get_employee(db.employees, id, e);
    unlock_record(db.employees, id);
    return ok;
}

static void modify_employee(int sock)
{
    Employee e;
    char value[NAME_LEN];
    int id, field;

    id = ask_int(sock, "Employee ID: ");
    if (!read_employee(id, &e)) {
        send_msg(sock, "No such employee.\n");
        return;
    }
    send_msg(sock, "Current: %s %s, %s, %s\n"
                   "1. First name\n2. Last name\n3. Reset password\n"
                   "4. Toggle status (ACTIVE/INACTIVE)\n5. Cancel\n",
             e.first, e.last, e.role, e.status);
    field = ask_int(sock, "Field to modify: ");
    if (field < 1 || field > 4)
        return;
    if (field == 1 || field == 2)
        ask(sock, "New value: ", value, NAME_LEN);
    else if (field == 3)
        ask_secret(sock, "New password: ", value, PASS_LEN);

    lock_record(db.employees, id, F_WRLCK);
    get_employee(db.employees, id, &e);
    if (field == 1)
        strcpy(e.first, value);
    else if (field == 2)
        strcpy(e.last, value);
    else if (field == 3)
        strcpy(e.password, value);
    else
        strcpy(e.status, strcmp(e.status, "ACTIVE") == 0 ? "INACTIVE" : "ACTIVE");
    put_employee(db.employees, &e);
    unlock_record(db.employees, id);
    send_msg(sock, "Employee %d updated (%s, %s).\n", id, e.role, e.status);
}

/* Counts loans an employee still has to decide on. */
static int open_loans_of(int emp_id)
{
    Loan l;
    int n, count = 0;

    lock_file(db.loans, F_RDLCK);
    n = record_count(db.loans);
    for (int i = 1; i <= n; i++)
        if (get_loan(db.loans, i, &l) && l.employee_id == emp_id &&
            (strcmp(l.status, "ASSIGNED") == 0 || strcmp(l.status, "IN_REVIEW") == 0))
            count++;
    unlock_file(db.loans);
    return count;
}

static void manage_roles(int sock)
{
    Employee e;
    int id, promote;

    list_employees(sock);
    id = ask_int(sock, "Employee ID: ");
    if (!read_employee(id, &e)) {
        send_msg(sock, "No such employee.\n");
        return;
    }
    promote = strcmp(e.role, "EMPLOYEE") == 0;
    send_msg(sock, "%s %s is a %s.\n", e.first, e.last, e.role);
    if (ask_int(sock, promote ? "1. Promote to MANAGER  2. Cancel : "
                              : "1. Demote to EMPLOYEE  2. Cancel : ") != 1)
        return;
    if (promote && open_loans_of(id) > 0) {
        send_msg(sock, "Employee still has open loan applications; cannot promote.\n");
        return;
    }

    lock_record(db.employees, id, F_WRLCK);
    get_employee(db.employees, id, &e);
    strcpy(e.role, promote ? "MANAGER" : "EMPLOYEE");
    put_employee(db.employees, &e);
    unlock_record(db.employees, id);
    send_msg(sock, "Employee %d is now a %s. (Takes effect at their next login.)\n", id, e.role);
}

static void change_admin_password(int sock, int id)
{
    char old[PASS_LEN], new1[PASS_LEN], new2[PASS_LEN];
    Admin a;

    ask_secret(sock, "Current password: ", old, sizeof old);
    ask_secret(sock, "New password: ", new1, sizeof new1);
    ask_secret(sock, "Confirm new password: ", new2, sizeof new2);
    if (strcmp(new1, new2) != 0) {
        send_msg(sock, "Passwords do not match.\n");
        return;
    }

    lock_record(db.admins, id, F_WRLCK);
    if (get_admin(db.admins, id, &a) && strcmp(a.password, old) == 0) {
        strcpy(a.password, new1);
        put_admin(db.admins, &a);
        send_msg(sock, "Password changed.\n");
    } else {
        send_msg(sock, "Current password is wrong.\n");
    }
    unlock_record(db.admins, id);
}

int admin_session(int sock, int id)
{
    for (;;) {
        send_msg(sock, "\n------ Administrator Menu ------\n"
                       "1. Add New Bank Employee\n"
                       "2. Modify Customer Details\n"
                       "3. Modify Employee Details\n"
                       "4. Manage User Roles\n"
                       "5. List Employees\n"
                       "6. Change Password\n"
                       "7. Logout\n"
                       "8. Exit\n");
        switch (ask_int(sock, "Choice: ")) {
        case 1: add_new_employee(sock); break;
        case 2: modify_customer(sock); break;
        case 3: modify_employee(sock); break;
        case 4: manage_roles(sock); break;
        case 5: list_employees(sock); break;
        case 6: change_admin_password(sock, id); break;
        case 7: return SESSION_LOGOUT;
        case 8: return SESSION_EXIT;
        default: send_msg(sock, "Invalid choice.\n");
        }
    }
}
