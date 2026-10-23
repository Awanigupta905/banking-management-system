// client.c
// Text menu client for all four roles (Customer, Employee, Manager, Admin).
// Every action opens a short TCP connection to the server, sends one request
// and prints the reply.
//
// Build:  gcc client.c -o client      (or just run: make)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP   "127.0.0.1"
#define PORT        8080
#define BUFFER_SIZE 4096
#define INPUT_SIZE  256

int current_user_id = -1;
int current_role_id = -1;   // 1=Customer, 2=Employee, 3=Manager, 4=Admin

/* ------------------------------------------------------------------ */
/*  Talking to the server                                              */
/* ------------------------------------------------------------------ */

void send_request(const char *request, char *response) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { strcpy(response, "ERROR|Could not create socket."); return; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        strcpy(response, "ERROR|Cannot connect to the server. Is it running?");
        close(sock_fd);
        return;
    }

    send(sock_fd, request, strlen(request), 0);

    // the server closes the connection after replying, so read until it does
    size_t total = 0;
    ssize_t n;
    while (total < BUFFER_SIZE - 1 && (n = recv(sock_fd, response + total, BUFFER_SIZE - 1 - total, 0)) > 0) {
        total += n;
    }
    response[total] = '\0';
    if (total == 0) strcpy(response, "ERROR|Server closed the connection.");

    close(sock_fd);
}

// Splits "SUCCESS|message" into status and message. Returns 1 for SUCCESS.
static int split_reply(char *response, char **message) {
    char *status = strtok(response, "|");
    *message = strtok(NULL, "");
    return status && strcmp(status, "SUCCESS") == 0;
}

// Sends a request, prints the answer. Returns 1 on success.
static int ask(const char *request) {
    char response[BUFFER_SIZE];
    char *msg;

    send_request(request, response);
    int ok = split_reply(response, &msg);

    if (!ok && msg && strcmp(msg, "SESSION_EXPIRED") == 0) {
        printf(">> Your session has ended. Please log in again.\n");
        current_user_id = -1;
        current_role_id = -1;
        return 0;
    }

    const char *label = ok ? "SUCCESS" : "ERROR";
    if (msg && strchr(msg, '\n')) {      // a table: print it nicely
        printf(">> %s:\n----------------------------------------------------\n%s"
               "----------------------------------------------------\n", label, msg);
    } else {
        printf(">> %s: %s\n", label, msg ? msg : "");
    }
    return ok;
}

// Builds "ACTION|role|user_id|args..." and sends it.
// Example: command("DEPOSIT", "%s", amount)
static int command(const char *action, const char *fmt, ...) {
    char args[BUFFER_SIZE / 2] = "";
    char request[BUFFER_SIZE];

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(args, sizeof args, fmt, ap);
        va_end(ap);
    }
    if (args[0]) snprintf(request, sizeof request, "%s|%d|%d|%s", action, current_role_id, current_user_id, args);
    else         snprintf(request, sizeof request, "%s|%d|%d", action, current_role_id, current_user_id);
    return ask(request);
}

// Logs the user out on the server. Also runs automatically when the program exits,
// so a customer who quits (or presses Ctrl+C) is never stuck as "already logged in".
static void do_logout(void) {
    if (current_user_id == -1) return;

    char request[128], response[BUFFER_SIZE];
    snprintf(request, sizeof request, "LOGOUT|%d|%d", current_role_id, current_user_id);
    send_request(request, response);
    current_user_id = -1;
    current_role_id = -1;
}

static void on_sigint(int sig) {
    printf("\n");
    exit(0);               // exit() runs do_logout() through atexit()
}

/* ------------------------------------------------------------------ */
/*  Reading input                                                      */
/* ------------------------------------------------------------------ */

static void read_line(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, size, stdin)) { printf("\n"); exit(0); }   // Ctrl+D / end of input

    if (!strchr(buf, '\n')) {              // line was longer than the buffer: throw the rest away
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }
    buf[strcspn(buf, "\n")] = '\0';
}

// A text that may contain spaces (e.g. feedback). The '|' is our separator, so it is not allowed.
static void read_text(const char *prompt, char *buf, int size) {
    while (1) {
        read_line(prompt, buf, size);
        if (buf[0] == '\0')          { printf("Input cannot be empty.\n"); continue; }
        if (strchr(buf, '|'))        { printf("The '|' character is not allowed.\n"); continue; }
        return;
    }
}

// A single word (username, password, id, amount): no spaces.
static void read_word(const char *prompt, char *buf, int size) {
    while (1) {
        read_text(prompt, buf, size);
        if (strchr(buf, ' ')) { printf("Spaces are not allowed here.\n"); continue; }
        return;
    }
}

int get_int_input(const char *prompt) {
    char buf[32], *end;
    read_line(prompt, buf, sizeof buf);
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') return -1;
    return (int)v;
}

/* ------------------------------------------------------------------ */
/*  Menus                                                              */
/* ------------------------------------------------------------------ */

void customer_menu(void) {
    char amount[INPUT_SIZE], account[INPUT_SIZE], pass[INPUT_SIZE], text[INPUT_SIZE];

    while (current_user_id != -1) {
        printf("\n--- Customer Menu (ID: %d) ---\n", current_user_id);
        printf("1. View Account Balance\n");
        printf("2. Deposit Money\n");
        printf("3. Withdraw Money\n");
        printf("4. Transfer Funds\n");
        printf("5. Apply for a Loan\n");
        printf("6. View My Loan Applications\n");
        printf("7. Change Password\n");
        printf("8. View Transaction History\n");
        printf("9. Add Feedback\n");
        printf("10. Logout\n");
        printf("11. Exit\n");

        switch (get_int_input("Enter choice: ")) {
            case 1:
                command("VIEW_BALANCE", NULL);
                break;
            case 2:
                read_word("Amount to deposit: ", amount, sizeof amount);
                command("DEPOSIT", "%s", amount);
                break;
            case 3:
                read_word("Amount to withdraw: ", amount, sizeof amount);
                command("WITHDRAW", "%s", amount);
                break;
            case 4:
                read_word("Target account number: ", account, sizeof account);
                read_word("Amount to transfer: ", amount, sizeof amount);
                command("TRANSFER", "%s|%s", account, amount);
                break;
            case 5:
                read_word("Loan amount: ", amount, sizeof amount);
                command("APPLY_LOAN", "%s", amount);
                break;
            case 6:
                command("MY_LOANS", NULL);
                break;
            case 7:
                read_word("New password: ", pass, sizeof pass);
                command("CHANGE_PASS", "%s", pass);
                break;
            case 8:
                command("VIEW_HISTORY", NULL);
                break;
            case 9:
                read_text("Feedback (5-255 characters): ", text, sizeof text);
                command("ADD_FEEDBACK", "%s", text);
                break;
            case 10:
                do_logout();
                printf("Logged out successfully.\n");
                return;
            case 11:
                printf("Goodbye.\n");
                exit(0);
            default:
                printf("Invalid choice.\n");
        }
    }
}

void employee_menu(void) {
    char username[INPUT_SIZE], pass[INPUT_SIZE], balance[INPUT_SIZE], id[INPUT_SIZE], answer[INPUT_SIZE];

    while (current_user_id != -1) {
        printf("\n--- Employee Menu (ID: %d) ---\n", current_user_id);
        printf("1. Add New Customer\n");
        printf("2. Modify Customer Details (Change Password)\n");
        printf("3. Process Loan Applications (Approve/Reject)\n");
        printf("4. View Customer Transactions (Passbook)\n");
        printf("5. View Assigned Loan Applications\n");
        printf("6. Change Password (Self)\n");
        printf("7. Logout\n");
        printf("8. Exit\n");

        switch (get_int_input("Enter choice: ")) {
            case 1:
                read_word("New customer username: ", username, sizeof username);
                read_word("New customer password: ", pass, sizeof pass);
                read_word("Initial balance: ", balance, sizeof balance);
                command("ADD_CUSTOMER", "%s|%s|%s", username, pass, balance);
                break;
            case 2:
                command("LIST_CUSTOMERS", NULL);
                read_word("Customer user ID to modify: ", id, sizeof id);
                read_word("New password for the customer: ", pass, sizeof pass);
                command("MOD_CUSTOMER", "%s|%s", id, pass);
                break;
            case 3:
                command("VIEW_ASSIGNED_LOANS", NULL);
                read_word("Loan ID to process: ", id, sizeof id);
                read_word("Approve or Reject? (A/R): ", answer, sizeof answer);
                if (answer[0] == 'A' || answer[0] == 'a')      command("PROCESS_LOAN", "%s|APPROVED", id);
                else if (answer[0] == 'R' || answer[0] == 'r') command("PROCESS_LOAN", "%s|REJECTED", id);
                else printf("Please type A or R.\n");
                break;
            case 4:
                command("LIST_CUSTOMERS", NULL);
                read_word("Customer user ID for passbook: ", id, sizeof id);
                command("VIEW_PASSBOOK", "%s", id);
                break;
            case 5:
                command("VIEW_ASSIGNED_LOANS", NULL);
                break;
            case 6:
                read_word("New password: ", pass, sizeof pass);
                command("CHANGE_PASS", "%s", pass);
                break;
            case 7:
                do_logout();
                printf("Logged out.\n");
                return;
            case 8:
                printf("Goodbye.\n");
                exit(0);
            default:
                printf("Invalid choice.\n");
        }
    }
}

void manager_menu(void) {
    char id[INPUT_SIZE], emp[INPUT_SIZE], answer[INPUT_SIZE], pass[INPUT_SIZE];

    while (current_user_id != -1) {
        printf("\n--- Manager Menu (ID: %d) ---\n", current_user_id);
        printf("1. Activate/Deactivate Customer Accounts\n");
        printf("2. Assign Loan Applications to Employees\n");
        printf("3. Review Customer Feedback\n");
        printf("4. Change Password\n");
        printf("5. Logout\n");
        printf("6. Exit\n");

        switch (get_int_input("Enter choice: ")) {
            case 1:
                command("LIST_CUSTOMERS", NULL);
                read_word("Customer user ID: ", id, sizeof id);
                read_word("Activate or Deactivate? (A/D): ", answer, sizeof answer);
                if (answer[0] == 'A' || answer[0] == 'a')      command("ACT_DEACT_ACC", "%s|ACTIVATE", id);
                else if (answer[0] == 'D' || answer[0] == 'd') command("ACT_DEACT_ACC", "%s|DEACTIVATE", id);
                else printf("Please type A or D.\n");
                break;
            case 2:
                // show the loans and the employees first, so the manager knows which ids to type
                if (!command("VIEW_LOANS", NULL)) break;
                command("LIST_EMPLOYEES", NULL);
                read_word("Loan ID to assign: ", id, sizeof id);
                read_word("Employee user ID: ", emp, sizeof emp);
                command("ASSIGN_LOAN", "%s|%s", id, emp);
                break;
            case 3:
                command("REVIEW_FEEDBACK", NULL);
                break;
            case 4:
                read_word("New password: ", pass, sizeof pass);
                command("CHANGE_PASS", "%s", pass);
                break;
            case 5:
                do_logout();
                printf("Logged out.\n");
                return;
            case 6:
                printf("Goodbye.\n");
                exit(0);
            default:
                printf("Invalid choice.\n");
        }
    }
}

void administrator_menu(void) {
    char username[INPUT_SIZE], pass[INPUT_SIZE], id[INPUT_SIZE], role[INPUT_SIZE];

    while (current_user_id != -1) {
        printf("\n--- Administrator Menu (ID: %d) ---\n", current_user_id);
        printf("1. Add New Bank Employee\n");
        printf("2. Modify Customer/Employee Details (Reset Password)\n");
        printf("3. Manage User Roles\n");
        printf("4. Change Password\n");
        printf("5. Logout\n");
        printf("6. Exit\n");

        switch (get_int_input("Enter choice: ")) {
            case 1:
                read_word("New employee username: ", username, sizeof username);
                read_word("New employee password: ", pass, sizeof pass);
                command("ADD_EMPLOYEE", "%s|%s", username, pass);
                break;
            case 2:
                command("LIST_USERS", NULL);
                read_word("Target user ID: ", id, sizeof id);
                read_word("New password: ", pass, sizeof pass);
                command("MOD_USER_DETAILS", "%s|%s", id, pass);
                break;
            case 3:
                command("LIST_USERS", NULL);
                read_word("Target user ID: ", id, sizeof id);
                read_word("New role (1=Customer 2=Employee 3=Manager 4=Admin): ", role, sizeof role);
                command("MANAGE_ROLES", "%s|%s", id, role);
                break;
            case 4:
                read_word("New password: ", pass, sizeof pass);
                command("CHANGE_PASS", "%s", pass);
                break;
            case 5:
                do_logout();
                printf("Logged out.\n");
                return;
            case 6:
                printf("Goodbye.\n");
                exit(0);
            default:
                printf("Invalid choice.\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Login screen                                                       */
/* ------------------------------------------------------------------ */

int main(void) {
    char username[INPUT_SIZE], password[INPUT_SIZE];
    char request[BUFFER_SIZE], response[BUFFER_SIZE];
    char *msg;

    atexit(do_logout);
    signal(SIGINT, on_sigint);

    while (1) {
        if (current_user_id == -1) {
            printf("\n--- Banking Management System ---\n");
            printf("Select Role to Login:\n");
            printf("1. Customer\n2. Bank Employee\n3. Manager\n4. Administrator\n5. Exit\n");
            int role_choice = get_int_input("Enter choice: ");

            if (role_choice == 5) break;
            if (role_choice < 1 || role_choice > 4) {
                printf("Invalid role choice.\n");
                continue;
            }

            if (role_choice == 1) {
                printf("\n--- Customer Access ---\n");
                printf("1. Existing Account Login\n");
                printf("2. Create New Account\n");
                int customer_choice = get_int_input("Enter choice: ");

                if (customer_choice == 2) {
                    printf("\n--- New Account Registration ---\n");
                    read_word("Choose a username (3-20 letters/digits): ", username, sizeof username);
                    read_word("Choose a password (min 4 characters): ", password, sizeof password);

                    snprintf(request, sizeof request, "CREATE_CUSTOMER|1|%s|%s", username, password);
                    send_request(request, response);

                    if (split_reply(response, &msg)) {
                        int new_acc = 0;
                        sscanf(msg, "%d|%d", &current_user_id, &new_acc);
                        current_role_id = 1;
                        printf("Account created. You are logged in as User ID %d (Account number %d).\n",
                               current_user_id, new_acc);
                    } else {
                        printf("Account creation failed: %s\n", msg ? msg : "unknown error");
                        continue;
                    }
                } else if (customer_choice != 1) {
                    printf("Invalid choice.\n");
                    continue;
                }
            }

            // normal login (skipped when we just created a new account)
            if (current_user_id == -1) {
                read_word("Username: ", username, sizeof username);
                read_word("Password: ", password, sizeof password);

                snprintf(request, sizeof request, "LOGIN|%d|%s|%s", role_choice, username, password);
                send_request(request, response);

                if (split_reply(response, &msg)) {
                    sscanf(msg, "%d|%d", &current_user_id, &current_role_id);
                    printf("Login successful. User ID: %d (Role %d)\n", current_user_id, current_role_id);
                } else {
                    printf("Login failed: %s\n", msg ? msg : "unknown error");
                }
            }
        }

        switch (current_role_id) {
            case 1: customer_menu(); break;
            case 2: employee_menu(); break;
            case 3: manager_menu(); break;
            case 4: administrator_menu(); break;
        }
    }
    return 0;
}
