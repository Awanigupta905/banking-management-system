// server.c
// Banking server: accepts a TCP connection, forks a child process for it,
// the child serves ONE request and exits.
// Data is kept in text files (see database.c) and protected with record locks.
//
// Build:  gcc server.c -o server      (or just run: make)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "database.c"      // Table, rec_lock(), user_find(), account_find_id() ...

#define PORT        8080
#define BACKLOG     10
#define BUFFER_SIZE 4096
#define OUT_SIZE    (BUFFER_SIZE - 64)   // room for the "SUCCESS|" prefix
#define MAX_TOKENS  10
#define MAX_AMOUNT  10000000.0
#define LOG_PATH    "bank.log"

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

// Build a reply like "SUCCESS|Deposited 50.00" or "ERROR|Invalid amount."
static void reply(char *resp, const char *status, const char *fmt, ...) {
    int n = snprintf(resp, BUFFER_SIZE, "%s|", status);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(resp + n, BUFFER_SIZE - n, fmt, ap);
    va_end(ap);
}
#define OK(resp, ...)  reply(resp, "SUCCESS", __VA_ARGS__)
#define ERR(resp, ...) reply(resp, "ERROR", __VA_ARGS__)

// Returns the amount rounded to 2 decimals, or -1 if the text is not a valid amount.
static double parse_amount(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return -1;
    if (!(v >= 0 && v <= MAX_AMOUNT)) return -1;       // also rejects NaN
    return round2(v);
}

static int valid_name(const char *s) {
    size_t len = strlen(s);
    if (len < 3 || len > 20) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return 1;
}

static int valid_password(const char *s) {
    return strlen(s) >= 4 && strlen(s) <= 30 && !strchr(s, '|');
}

/* ---- printing lists ---- */

typedef struct {
    char text[OUT_SIZE];
    int rows;
} Output;

static void out_add(Output *o, const char *fmt, ...) {
    size_t len = strlen(o->text);
    if (len >= OUT_SIZE - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->text + len, OUT_SIZE - len, fmt, ap);
    va_end(ap);
    o->rows++;
}

static void send_list(char *resp, Output *o, const char *heading, const char *empty_msg) {
    if (o->rows == 0) OK(resp, "%s", empty_msg);
    else              OK(resp, "%s\n%s", heading, o->text);
}

/* ---- money ---- */

// Adds delta (can be negative) to one account while holding that record's lock.
// Returns 1 = done, 0 = not enough money, -1 = account not found.
static int change_balance(int account_id, double delta) {
    Account a;
    int idx = account_find_id(account_id, &a);
    if (idx < 0) return -1;

    rec_lock(&accounts_t, idx);
    account_get(idx, &a);                       // read AGAIN, now that we hold the lock
    double new_balance = round2(a.balance + delta);
    int result = 1;
    if (new_balance < 0) {
        result = 0;
    } else {
        a.balance = new_balance;
        if (account_put(idx, &a) != 0) result = -1;
    }
    rec_unlock(&accounts_t, idx);
    return result;
}

// Creates a user (and a bank account for customers). Returns 0 = ok, -1 = username taken, -2 = error.
// The user file is locked as a whole, so two people cannot grab the same username or id.
static int create_user(const char *name, const char *pass, int role, int logged_in,
                       double opening_balance, int *new_uid, int *new_acc) {
    User u, tmp;
    int result = 0;

    table_lock(&users_t);
    table_lock(&accounts_t);

    if (user_find_name(name, &tmp) >= 0) {
        result = -1;
    } else {
        int max_id = 100;                       // first new user gets 101 or more
        for (int i = 0; i < rec_count(&users_t); i++) {
            if (user_get(i, &tmp) == 0 && tmp.id > max_id) max_id = tmp.id;
        }
        u.id = max_id + 1;
        snprintf(u.name, sizeof u.name, "%s", name);
        snprintf(u.pass, sizeof u.pass, "%s", pass);
        u.role = role;
        u.active = 1;
        u.logged_in = logged_in;

        if (user_put(rec_count(&users_t), &u) != 0) result = -2;
        *new_uid = u.id;
        *new_acc = -1;

        if (result == 0 && role == 1) {
            Account a, t;
            int max_acc = 10000;
            for (int i = 0; i < rec_count(&accounts_t); i++) {
                if (account_get(i, &t) == 0 && t.id > max_acc) max_acc = t.id;
            }
            a.id = max_acc + 1;
            a.user_id = u.id;
            a.balance = opening_balance;
            if (account_put(rec_count(&accounts_t), &a) != 0) result = -2;
            *new_acc = a.id;
        }
    }

    table_unlock(&accounts_t);
    table_unlock(&users_t);

    if (result == 0 && role == 1 && opening_balance > 0) txn_add(*new_acc, "DEPOSIT", opening_balance, "Opening balance");
    return result;
}

// Changes the logged_in flag of a user under the record lock.
// If `expected` is 0 or 1 the change only happens when the flag currently has that value.
// Returns 1 if the flag was changed.
static int set_logged_in(int uid, int expected, int new_value) {
    User u;
    int idx = user_find(uid, &u);
    if (idx < 0) return 0;

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    int changed = 0;
    if (expected < 0 || u.logged_in == expected) {
        u.logged_in = new_value;
        changed = (user_put(idx, &u) == 0);
    }
    rec_unlock(&users_t, idx);
    return changed;
}

/* ------------------------------------------------------------------ */
/*  Commands for every role                                            */
/*  Each one gets: the logged-in user id, its arguments, the reply.    */
/* ------------------------------------------------------------------ */

/* ----- common to all roles ----- */

static void cmd_change_pass(int uid, char **a, char *resp) {
    if (!valid_password(a[0])) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }

    User u;
    int idx = user_find(uid, &u);
    if (idx < 0) { ERR(resp, "User not found."); return; }

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    snprintf(u.pass, sizeof u.pass, "%s", a[0]);
    int ok = (user_put(idx, &u) == 0);
    rec_unlock(&users_t, idx);

    if (ok) OK(resp, "Password changed successfully.");
    else    ERR(resp, "Failed to change password.");
}

/* ----- customer ----- */

static void cmd_view_balance(int uid, char **a, char *resp) {
    Account acc;
    if (account_find_user(uid, &acc) >= 0) OK(resp, "Account %d balance: %.2f", acc.id, acc.balance);
    else                                   ERR(resp, "Account not found.");
}

static void cmd_deposit(int uid, char **a, char *resp) {
    double amt = parse_amount(a[0]);
    Account acc;

    if (amt <= 0) { ERR(resp, "Invalid deposit amount."); return; }
    if (account_find_user(uid, &acc) < 0) { ERR(resp, "Account not found."); return; }

    if (change_balance(acc.id, amt) == 1) {
        txn_add(acc.id, "DEPOSIT", amt, "Cash deposit");
        OK(resp, "Deposited %.2f.", amt);
    } else {
        ERR(resp, "Deposit failed.");
    }
}

static void cmd_withdraw(int uid, char **a, char *resp) {
    double amt = parse_amount(a[0]);
    Account acc;

    if (amt <= 0) { ERR(resp, "Invalid withdrawal amount."); return; }
    if (account_find_user(uid, &acc) < 0) { ERR(resp, "Account not found."); return; }

    // the "enough money?" check happens INSIDE change_balance, while the record is locked
    int r = change_balance(acc.id, -amt);
    if (r == 1) {
        txn_add(acc.id, "WITHDRAW", amt, "Cash withdrawal");
        OK(resp, "Withdrew %.2f.", amt);
    } else if (r == 0) {
        ERR(resp, "Insufficient funds.");
    } else {
        ERR(resp, "Withdrawal failed.");
    }
}

static void cmd_transfer(int uid, char **a, char *resp) {
    int to_id = atoi(a[0]);
    double amt = parse_amount(a[1]);
    Account from, to;

    if (amt <= 0 || to_id <= 0) { ERR(resp, "Invalid account number or amount."); return; }

    int from_idx = account_find_user(uid, &from);
    int to_idx = account_find_id(to_id, &to);
    if (from_idx < 0) { ERR(resp, "Account not found."); return; }
    if (to_idx < 0)   { ERR(resp, "Target account not found."); return; }
    if (to_id == from.id) { ERR(resp, "You cannot transfer to your own account."); return; }

    User target_user;
    if (user_find(to.user_id, &target_user) < 0 || !target_user.active) {
        ERR(resp, "Target account is inactive.");
        return;
    }

    // Lock BOTH accounts. Always lock the smaller record number first: if two
    // transfers go in opposite directions (A->B and B->A) they ask for the locks in
    // the same order, so they can never wait for each other forever (deadlock).
    int first = from_idx < to_idx ? from_idx : to_idx;
    int second = from_idx < to_idx ? to_idx : from_idx;
    rec_lock(&accounts_t, first);
    rec_lock(&accounts_t, second);

    account_get(from_idx, &from);               // read again now that both are locked
    account_get(to_idx, &to);

    int status;                                  // 1 = done, 0 = no money, -1 = error
    if (from.balance + 0.001 < amt) {
        status = 0;
    } else {
        Account old_from = from;
        from.balance = round2(from.balance - amt);
        to.balance = round2(to.balance + amt);

        status = 1;
        if (account_put(from_idx, &from) != 0) {
            status = -1;
        } else if (account_put(to_idx, &to) != 0) {
            account_put(from_idx, &old_from);    // undo the first write -> all or nothing
            status = -1;
        }
    }

    rec_unlock(&accounts_t, second);
    rec_unlock(&accounts_t, first);

    if (status == 1) {
        char d1[48], d2[48];
        snprintf(d1, sizeof d1, "To account %d", to.id);
        snprintf(d2, sizeof d2, "From account %d", from.id);
        txn_add(from.id, "TRANSFER_OUT", amt, d1);
        txn_add(to.id, "TRANSFER_IN", amt, d2);
        OK(resp, "Transferred %.2f to account %d.", amt, to_id);
    } else if (status == 0) {
        ERR(resp, "Insufficient funds.");
    } else {
        ERR(resp, "Transfer failed.");
    }
}

// Shows the last `limit` transactions of a customer (used for history and passbook).
static void show_statement(int uid, int limit, char *resp) {
    Account acc;
    if (account_find_user(uid, &acc) < 0) { ERR(resp, "No customer account found for user %d.", uid); return; }

    Output out = {"", 0};
    Txn t;
    int shown = 0;
    for (int i = rec_count(&txns_t) - 1; i >= 0 && shown < limit; i--) {     // newest first
        if (txn_get(i, &t) == 0 && t.account_id == acc.id) {
            out_add(&out, "%s | %s | %.2f | %s\n", t.time, t.type, t.amount, t.details);
            shown++;
        }
    }

    char heading[200], empty[200];
    snprintf(heading, sizeof heading, "Account %d - Balance: %.2f\nTime | Type | Amount | Details", acc.id, acc.balance);
    snprintf(empty, sizeof empty, "Account %d (balance %.2f) has no transactions yet.", acc.id, acc.balance);
    send_list(resp, &out, heading, empty);
}

static void cmd_view_history(int uid, char **a, char *resp) {
    show_statement(uid, 10, resp);
}

static void cmd_apply_loan(int uid, char **a, char *resp) {
    double amt = parse_amount(a[0]);
    Account acc;

    if (amt <= 0) { ERR(resp, "Invalid loan amount."); return; }
    if (account_find_user(uid, &acc) < 0) { ERR(resp, "Account not found."); return; }

    table_lock(&loans_t);                        // so two applications never get the same loan id
    Loan l;
    int idx = rec_count(&loans_t);
    l.id = idx + 1;
    l.account_id = acc.id;
    l.amount = amt;
    snprintf(l.status, sizeof l.status, "PENDING");
    l.emp_id = 0;
    int ok = (loan_put(idx, &l) == 0);
    table_unlock(&loans_t);

    if (ok) OK(resp, "Loan application #%d for %.2f submitted. Status: PENDING.", l.id, amt);
    else    ERR(resp, "Loan application failed.");
}

static void cmd_my_loans(int uid, char **a, char *resp) {
    Account acc;
    if (account_find_user(uid, &acc) < 0) { ERR(resp, "Account not found."); return; }

    Output out = {"", 0};
    Loan l;
    for (int i = 0; i < rec_count(&loans_t); i++) {
        if (loan_get(i, &l) == 0 && l.account_id == acc.id) out_add(&out, "%d | %.2f | %s\n", l.id, l.amount, l.status);
    }
    send_list(resp, &out, "Loan | Amount | Status", "You have no loan applications.");
}

static void cmd_add_feedback(int uid, char **a, char *resp) {
    size_t len = strlen(a[0]);
    if (len < 5 || len > 255) { ERR(resp, "Feedback must be 5-255 characters."); return; }

    char line[512], when[24];
    now_string(when, sizeof when);

    table_lock(&feedback_t);
    int idx = rec_count(&feedback_t);
    snprintf(line, sizeof line, "%d|%d|%s|%s", idx + 1, uid, when, a[0]);
    int ok = (rec_write(&feedback_t, idx, line) == 0);
    table_unlock(&feedback_t);

    if (ok) OK(resp, "Feedback submitted. Thank you.");
    else    ERR(resp, "Feedback submission failed.");
}

/* ----- bank employee ----- */

static void cmd_list_customers(int uid, char **a, char *resp) {
    Output out = {"", 0};
    User u;
    Account acc;
    for (int i = 0; i < rec_count(&users_t); i++) {
        if (user_get(i, &u) != 0 || u.role != 1) continue;
        if (account_find_user(u.id, &acc) >= 0) out_add(&out, "%d | %s | %d | %s\n", u.id, u.name, acc.id, u.active ? "active" : "deactivated");
        else                                    out_add(&out, "%d | %s | - | %s\n", u.id, u.name, u.active ? "active" : "deactivated");
    }
    send_list(resp, &out, "User ID | Username | Account | Status", "No customers found.");
}

static void cmd_add_customer(int uid, char **a, char *resp) {
    double balance = parse_amount(a[2]);
    int new_uid, new_acc;

    if (!valid_name(a[0]))     { ERR(resp, "Username must be 3-20 letters, digits or _."); return; }
    if (!valid_password(a[1])) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }
    if (balance < 0)           { ERR(resp, "Invalid initial balance."); return; }

    int r = create_user(a[0], a[1], 1, 0, balance, &new_uid, &new_acc);
    if (r == 0)       OK(resp, "Customer created. User ID: %d, Account: %d.", new_uid, new_acc);
    else if (r == -1) ERR(resp, "Username already exists.");
    else              ERR(resp, "Could not create customer.");
}

static void cmd_mod_customer(int uid, char **a, char *resp) {
    int target = atoi(a[0]);
    if (!valid_password(a[1])) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }

    User u;
    int idx = user_find(target, &u);
    if (idx < 0 || u.role != 1) { ERR(resp, "Customer not found."); return; }

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    snprintf(u.pass, sizeof u.pass, "%s", a[1]);
    user_put(idx, &u);
    rec_unlock(&users_t, idx);

    OK(resp, "Customer %d password changed.", target);
}

static void cmd_view_passbook(int uid, char **a, char *resp) {
    show_statement(atoi(a[0]), 20, resp);
}

static void cmd_view_assigned_loans(int uid, char **a, char *resp) {
    Output out = {"", 0};
    Loan l;
    for (int i = 0; i < rec_count(&loans_t); i++) {
        if (loan_get(i, &l) == 0 && l.emp_id == uid && strcmp(l.status, "ASSIGNED") == 0)
            out_add(&out, "%d | %d | %.2f | %s\n", l.id, l.account_id, l.amount, l.status);
    }
    send_list(resp, &out, "Loan | Account | Amount | Status", "No loans are assigned to you right now.");
}

static void cmd_process_loan(int uid, char **a, char *resp) {
    int loan_id = atoi(a[0]);
    int approve;
    Loan l;

    if (strcmp(a[1], "APPROVED") == 0)      approve = 1;
    else if (strcmp(a[1], "REJECTED") == 0) approve = 0;
    else { ERR(resp, "Decision must be APPROVED or REJECTED."); return; }

    int idx = loan_find(loan_id, &l);
    if (idx < 0) { ERR(resp, "Loan not found."); return; }

    rec_lock(&loans_t, idx);                     // nobody else can process this loan now
    loan_get(idx, &l);

    if (strcmp(l.status, "ASSIGNED") != 0 || l.emp_id != uid) {
        rec_unlock(&loans_t, idx);
        ERR(resp, "Loan is not assigned to you, or already processed.");
        return;
    }

    int ok = 1;
    snprintf(l.status, sizeof l.status, "%s", approve ? "APPROVED" : "REJECTED");
    loan_put(idx, &l);                           // mark it first...

    if (approve) {                               // ...then pay out; if paying fails, mark it back
        if (change_balance(l.account_id, l.amount) == 1) {
            txn_add(l.account_id, "LOAN_CREDIT", l.amount, "Loan approved");
        } else {
            snprintf(l.status, sizeof l.status, "ASSIGNED");
            loan_put(idx, &l);
            ok = 0;
        }
    }
    rec_unlock(&loans_t, idx);

    if (ok) OK(resp, "Loan %d %s.", loan_id, approve ? "APPROVED" : "REJECTED");
    else    ERR(resp, "Could not process the loan.");
}

/* ----- manager ----- */

static void cmd_act_deact(int uid, char **a, char *resp) {
    int target = atoi(a[0]);
    int activate;
    User u;

    if (strcmp(a[1], "ACTIVATE") == 0)        activate = 1;
    else if (strcmp(a[1], "DEACTIVATE") == 0) activate = 0;
    else { ERR(resp, "Status must be ACTIVATE or DEACTIVATE."); return; }

    int idx = user_find(target, &u);
    if (idx < 0 || u.role != 1) { ERR(resp, "Customer not found."); return; }

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    u.active = activate;
    if (!activate) u.logged_in = 0;              // throw the customer out immediately
    user_put(idx, &u);
    rec_unlock(&users_t, idx);

    OK(resp, "Customer %d: %s done.", target, a[1]);
}

static void cmd_view_loans(int uid, char **a, char *resp) {
    Output out = {"", 0};
    Loan l;
    for (int i = 0; i < rec_count(&loans_t); i++) {
        if (loan_get(i, &l) != 0) continue;
        if (strcmp(l.status, "PENDING") != 0 && strcmp(l.status, "ASSIGNED") != 0) continue;
        if (l.emp_id > 0) out_add(&out, "%d | %d | %.2f | %s | %d\n", l.id, l.account_id, l.amount, l.status, l.emp_id);
        else              out_add(&out, "%d | %d | %.2f | %s | -\n", l.id, l.account_id, l.amount, l.status);
    }
    send_list(resp, &out, "Loan | Account | Amount | Status | Employee", "No pending loan applications.");
}

static void cmd_list_employees(int uid, char **a, char *resp) {
    Output out = {"", 0};
    User u;
    for (int i = 0; i < rec_count(&users_t); i++) {
        if (user_get(i, &u) == 0 && u.role == 2 && u.active) out_add(&out, "%d | %s\n", u.id, u.name);
    }
    send_list(resp, &out, "User ID | Username", "No active employees found.");
}

static void cmd_assign_loan(int uid, char **a, char *resp) {
    int loan_id = atoi(a[0]);
    int emp_id = atoi(a[1]);
    User emp;
    Loan l;

    if (user_find(emp_id, &emp) < 0 || emp.role != 2 || !emp.active) {
        ERR(resp, "That user is not an active bank employee.");
        return;
    }
    int idx = loan_find(loan_id, &l);
    if (idx < 0) { ERR(resp, "Loan not found."); return; }

    rec_lock(&loans_t, idx);
    loan_get(idx, &l);
    int ok = (strcmp(l.status, "PENDING") == 0 || strcmp(l.status, "ASSIGNED") == 0);
    if (ok) {
        l.emp_id = emp_id;
        snprintf(l.status, sizeof l.status, "ASSIGNED");
        loan_put(idx, &l);
    }
    rec_unlock(&loans_t, idx);

    if (ok) OK(resp, "Loan %d assigned to employee %d.", loan_id, emp_id);
    else    ERR(resp, "Loan %d is already processed.", loan_id);
}

static void cmd_review_feedback(int uid, char **a, char *resp) {
    Output out = {"", 0};
    Feedback fb;
    int shown = 0;
    for (int i = rec_count(&feedback_t) - 1; i >= 0 && shown < 20; i--) {      // newest first
        if (feedback_get(i, &fb) == 0) {
            out_add(&out, "%d | %d | %s | %s\n", fb.id, fb.user_id, fb.time, fb.msg);
            shown++;
        }
    }
    send_list(resp, &out, "ID | User | Time | Message", "No feedback yet.");
}

/* ----- administrator ----- */

static void cmd_list_users(int uid, char **a, char *resp) {
    static const char *role_names[] = {"?", "Customer", "Employee", "Manager", "Admin"};
    Output out = {"", 0};
    User u;
    for (int i = 0; i < rec_count(&users_t); i++) {
        if (user_get(i, &u) == 0)
            out_add(&out, "%d | %s | %s | %s\n", u.id, u.name, role_names[u.role >= 1 && u.role <= 4 ? u.role : 0], u.active ? "active" : "deactivated");
    }
    send_list(resp, &out, "User ID | Username | Role | Status", "No users found.");
}

static void cmd_add_employee(int uid, char **a, char *resp) {
    int new_uid, new_acc;

    if (!valid_name(a[0]))     { ERR(resp, "Username must be 3-20 letters, digits or _."); return; }
    if (!valid_password(a[1])) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }

    int r = create_user(a[0], a[1], 2, 0, 0, &new_uid, &new_acc);
    if (r == 0)       OK(resp, "New bank employee created. User ID: %d.", new_uid);
    else if (r == -1) ERR(resp, "Username already exists.");
    else              ERR(resp, "Could not create employee.");
}

static void cmd_mod_user(int uid, char **a, char *resp) {
    int target = atoi(a[0]);
    if (!valid_password(a[1])) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }

    User u;
    int idx = user_find(target, &u);
    if (idx < 0) { ERR(resp, "User not found."); return; }

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    snprintf(u.pass, sizeof u.pass, "%s", a[1]);
    user_put(idx, &u);
    rec_unlock(&users_t, idx);

    OK(resp, "Password of user %d changed.", target);
}

static void cmd_manage_roles(int uid, char **a, char *resp) {
    int target = atoi(a[0]);
    int new_role = atoi(a[1]);
    User u;

    if (target <= 0 || new_role < 1 || new_role > 4) { ERR(resp, "Invalid user id or role (use 1-4)."); return; }
    if (target == uid) { ERR(resp, "You cannot change your own role."); return; }

    int idx = user_find(target, &u);
    if (idx < 0) { ERR(resp, "User not found."); return; }

    rec_lock(&users_t, idx);
    user_get(idx, &u);
    u.role = new_role;
    u.logged_in = 0;                             // forces a fresh login with the new role
    user_put(idx, &u);
    rec_unlock(&users_t, idx);

    // a customer needs a bank account; create one if the user never had it
    Account acc;
    if (new_role == 1 && account_find_user(target, &acc) < 0) {
        table_lock(&accounts_t);
        if (account_find_user(target, &acc) < 0) {          // check again inside the lock
            int max_acc = 10000;
            Account t;
            for (int i = 0; i < rec_count(&accounts_t); i++) {
                if (account_get(i, &t) == 0 && t.id > max_acc) max_acc = t.id;
            }
            acc.id = max_acc + 1;
            acc.user_id = target;
            acc.balance = 0;
            account_put(rec_count(&accounts_t), &acc);
        }
        table_unlock(&accounts_t);
    }

    OK(resp, "User %d is now role %d.", target, new_role);
}

/* ------------------------------------------------------------------ */
/*  Login / logout / sign-up (these need no existing session)          */
/* ------------------------------------------------------------------ */

static void do_login(int role, const char *username, const char *password, char *resp) {
    User u;

    if (user_find_name(username, &u) < 0 || strcmp(u.pass, password) != 0) {
        ERR(resp, "Invalid username or password.");
        return;
    }
    if (u.role != role) { ERR(resp, "This user does not belong to the selected role."); return; }
    if (!u.active)      { ERR(resp, "Account deactivated. Please contact the bank."); return; }

    // One session per user: the flag is only switched on if it is still 0.
    // The check and the change happen under the record lock, so two
    // simultaneous logins cannot both succeed.
    if (set_logged_in(u.id, 0, 1)) OK(resp, "%d|%d", u.id, u.role);
    else                           ERR(resp, "This user is already logged in somewhere else.");
}

static void do_create_customer(const char *username, const char *password, char *resp) {
    int new_uid, new_acc;

    if (!valid_name(username))     { ERR(resp, "Username must be 3-20 letters, digits or _."); return; }
    if (!valid_password(password)) { ERR(resp, "Password must be 4-30 characters (no '|')."); return; }

    // new customer starts logged in (logged_in = 1)
    int r = create_user(username, password, 1, 1, 0, &new_uid, &new_acc);
    if (r == 0)       OK(resp, "%d|%d", new_uid, new_acc);
    else if (r == -1) ERR(resp, "Username already taken.");
    else              ERR(resp, "Could not create the account.");
}

/* ------------------------------------------------------------------ */
/*  Table of commands: name, which role may use it, argument count     */
/*  (role 0 = every role)                                              */
/* ------------------------------------------------------------------ */

typedef void (*Handler)(int uid, char **args, char *resp);

typedef struct {
    const char *name;
    int role;
    int nargs;
    Handler fn;
} Command;

static const Command commands[] = {
    // everyone
    {"CHANGE_PASS",          0, 1, cmd_change_pass},
    // customer
    {"VIEW_BALANCE",         1, 0, cmd_view_balance},
    {"DEPOSIT",              1, 1, cmd_deposit},
    {"WITHDRAW",             1, 1, cmd_withdraw},
    {"TRANSFER",             1, 2, cmd_transfer},
    {"VIEW_HISTORY",         1, 0, cmd_view_history},
    {"APPLY_LOAN",           1, 1, cmd_apply_loan},
    {"MY_LOANS",             1, 0, cmd_my_loans},
    {"ADD_FEEDBACK",         1, 1, cmd_add_feedback},
    // bank employee
    {"LIST_CUSTOMERS",       2, 0, cmd_list_customers},
    {"ADD_CUSTOMER",         2, 3, cmd_add_customer},
    {"MOD_CUSTOMER",         2, 2, cmd_mod_customer},
    {"VIEW_PASSBOOK",        2, 1, cmd_view_passbook},
    {"VIEW_ASSIGNED_LOANS",  2, 0, cmd_view_assigned_loans},
    {"PROCESS_LOAN",         2, 2, cmd_process_loan},
    // manager
    {"LIST_CUSTOMERS",       3, 0, cmd_list_customers},
    {"ACT_DEACT_ACC",        3, 2, cmd_act_deact},
    {"VIEW_LOANS",           3, 0, cmd_view_loans},
    {"LIST_EMPLOYEES",       3, 0, cmd_list_employees},
    {"ASSIGN_LOAN",          3, 2, cmd_assign_loan},
    {"REVIEW_FEEDBACK",      3, 0, cmd_review_feedback},
    // administrator
    {"LIST_USERS",           4, 0, cmd_list_users},
    {"ADD_EMPLOYEE",         4, 2, cmd_add_employee},
    {"MOD_USER_DETAILS",     4, 2, cmd_mod_user},
    {"MANAGE_ROLES",         4, 2, cmd_manage_roles},
};
#define NUM_COMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

/* ------------------------------------------------------------------ */
/*  Handling one request                                               */
/*  Request format:  ACTION|role|user_id|arg1|arg2...                  */
/*  (LOGIN and CREATE_CUSTOMER carry username|password instead of id)  */
/* ------------------------------------------------------------------ */

static void process_request(char *buffer, char *resp, int *log_uid, const char **log_action) {
    char *t[MAX_TOKENS];
    int n = 0;

    for (char *p = strtok(buffer, "|"); p && n < MAX_TOKENS; p = strtok(NULL, "|")) t[n++] = p;
    if (n < 3) { ERR(resp, "Malformed request."); return; }

    const char *action = t[0];
    int role = atoi(t[1]);
    *log_action = action;

    if (strcmp(action, "LOGIN") == 0) {
        if (n == 4) do_login(role, t[2], t[3], resp);
        else        ERR(resp, "Malformed request.");
        return;
    }
    if (strcmp(action, "CREATE_CUSTOMER") == 0) {
        if (n == 4 && role == 1) do_create_customer(t[2], t[3], resp);
        else                     ERR(resp, "Malformed request.");
        return;
    }

    // From here on the user must already be logged in.
    int uid = atoi(t[2]);
    User user;
    *log_uid = uid;

    if (user_find(uid, &user) < 0) { ERR(resp, "Unknown user."); return; }

    if (strcmp(action, "LOGOUT") == 0) {
        set_logged_in(uid, -1, 0);
        OK(resp, "Session terminated.");
        return;
    }

    if (!user.logged_in)    { ERR(resp, "SESSION_EXPIRED"); return; }
    if (!user.active)       { ERR(resp, "Account deactivated."); return; }
    // We trust the role stored in the file, never the one sent by the client.
    if (user.role != role)  { ERR(resp, "Role mismatch."); return; }

    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(commands[i].name, action) == 0 && (commands[i].role == 0 || commands[i].role == user.role)) {
            if (n - 3 != commands[i].nargs) { ERR(resp, "Wrong number of arguments."); return; }
            commands[i].fn(uid, &t[3], resp);
            return;
        }
    }
    ERR(resp, "Unknown action, or not allowed for your role.");
}

// Writes one line to the screen and to bank.log using plain system calls.
// O_APPEND makes every write() go to the end of the file, even when several
// processes write at the same time.
static void log_event(int uid, const char *action, const char *resp) {
    char line[256], when[32];
    now_string(when, sizeof when);

    int len = snprintf(line, sizeof line, "%s pid=%d user=%d action=%s result=%s\n",
                       when, getpid(), uid, action, strncmp(resp, "SUCCESS", 7) == 0 ? "OK" : "FAIL");
    if (len > (int)sizeof line - 1) len = sizeof line - 1;

    write(STDOUT_FILENO, line, len);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, line, len);
        close(fd);
    }
}

// This is what the child process runs: read ONE request, answer it, exit.
static void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    char resp[BUFFER_SIZE] = "ERROR|Unknown request.";
    int uid = -1;
    const char *action = "UNKNOWN";

    ssize_t got = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (got > 0) {
        buffer[got] = '\0';
        if (db_open() == 0) {                 // this process opens its OWN descriptors
            process_request(buffer, resp, &uid, &action);
            db_close();
        } else {
            ERR(resp, "Server data error.");
        }
    }

    size_t sent = 0, total = strlen(resp);
    while (sent < total) {
        ssize_t w = send(client_fd, resp + sent, total - sent, 0);
        if (w <= 0) break;
        sent += w;
    }
    close(client_fd);

    log_event(uid, action, resp);
    exit(0);
}

// Without this, finished children stay behind as zombies.
static void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) { perror("sigaction"); return 1; }

    if (db_init() != 0) return 1;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) { perror("bind"); return 1; }
    if (listen(sockfd, BACKLOG) == -1) { perror("listen"); return 1; }

    printf("Server listening on port %d ...\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t size = sizeof(client_addr);
        int client_fd = accept(sockfd, (struct sockaddr *)&client_addr, &size);
        if (client_fd == -1) { perror("accept"); continue; }

        pid_t pid = fork();
        if (pid == 0) {              // child
            close(sockfd);
            handle_client(client_fd);
        }
        close(client_fd);            // parent: the child owns this socket now
    }
    return 0;
}
