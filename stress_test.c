// stress_test.c
// Race-condition demo for the viva. Start the server first, then run ./stress_test
//
// It forks many child processes that hit the server AT THE SAME TIME and then
// checks that no money was lost, created or double-spent.
//
// Build:  gcc stress_test.c -o stress_test      (or: make)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT        8080
#define BUFFER_SIZE 4096

static int passed = 0, failed = 0;

static void request(const char *req, char *resp) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        strcpy(resp, "ERROR|cannot connect");
        close(fd);
        return;
    }
    send(fd, req, strlen(req), 0);

    size_t total = 0;
    ssize_t n;
    while (total < BUFFER_SIZE - 1 && (n = recv(fd, resp + total, BUFFER_SIZE - 1 - total, 0)) > 0) total += n;
    resp[total] = '\0';
    close(fd);
}

static int is_ok(const char *resp) {
    return strncmp(resp, "SUCCESS", 7) == 0;
}

static int login(int role, const char *user, const char *pass) {
    char req[128], resp[BUFFER_SIZE];
    int uid = -1, r = 0;
    snprintf(req, sizeof req, "LOGIN|%d|%s|%s", role, user, pass);
    request(req, resp);
    if (is_ok(resp)) sscanf(resp, "SUCCESS|%d|%d", &uid, &r);
    return uid;
}

static void logout(int role, int uid) {
    char req[64], resp[BUFFER_SIZE];
    snprintf(req, sizeof req, "LOGOUT|%d|%d", role, uid);
    request(req, resp);
}

static double balance_of(int uid) {
    char req[64], resp[BUFFER_SIZE];
    snprintf(req, sizeof req, "VIEW_BALANCE|1|%d", uid);
    request(req, resp);
    char *p = strstr(resp, "balance: ");
    return p ? atof(p + 9) : -1;
}

// Starts `count` child processes that all send the same request at once.
// Returns how many of them got a SUCCESS reply.
static int run_parallel(int count, const char *req) {
    for (int i = 0; i < count; i++) {
        if (fork() == 0) {
            char resp[BUFFER_SIZE];
            request(req, resp);
            _exit(is_ok(resp) ? 0 : 1);
        }
    }
    int successes = 0, status;
    while (wait(&status) > 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) successes++;
    }
    return successes;
}

static void check(const char *name, int condition) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (condition) passed++; else failed++;
}

int main(void) {
    char req[128];

    printf("TEST 1: one session per user (5 simultaneous logins as emp1)\n");
    int wins = run_parallel(5, "LOGIN|2|emp1|pass");
    printf("  %d of 5 logins succeeded\n", wins);
    check("exactly one login succeeded", wins == 1);
    logout(2, 201);

    int c1 = login(1, "cust1", "pass");
    int c2 = login(1, "cust2", "pass2");
    if (c1 < 0 || c2 < 0) {
        printf("Could not log in the test customers (are cust1/cust2 already logged in? is the server running?)\n");
        return 1;
    }

    printf("\nTEST 2: 20 simultaneous deposits of 10.00 into cust1\n");
    double before = balance_of(c1);
    int done = run_parallel(20, "DEPOSIT|1|101|10");
    double after = balance_of(c1);
    printf("  balance %.2f -> %.2f (%d deposits succeeded)\n", before, after, done);
    check("every deposit counted (no lost update)", fabs(after - (before + done * 10.0)) < 0.001 && done == 20);

    printf("\nTEST 3: 10 simultaneous withdrawals of 50.00 from cust2\n");
    run_parallel(1, "DEPOSIT|1|102|175");                  // make sure there is some money to fight over
    before = balance_of(c2);
    int expected = (int)(before / 50.0);
    if (expected > 10) expected = 10;
    done = run_parallel(10, "WITHDRAW|1|102|50");
    after = balance_of(c2);
    printf("  balance %.2f -> %.2f (%d withdrawals succeeded, %d expected)\n", before, after, done, expected);
    check("only as many withdrawals as the balance allows", done == expected);
    check("balance is correct and not negative", fabs(after - (before - done * 50.0)) < 0.001 && after >= 0);

    printf("\nTEST 4: 10 simultaneous transfers of 5.00 from cust1 to cust2 (account 10002)\n");
    double a1 = balance_of(c1), a2 = balance_of(c2);
    snprintf(req, sizeof req, "TRANSFER|1|%d|10002|5", c1);
    done = run_parallel(10, req);
    double b1 = balance_of(c1), b2 = balance_of(c2);
    printf("  total money before: %.2f, after: %.2f (%d transfers succeeded)\n", a1 + a2, b1 + b2, done);
    check("no money created or destroyed", fabs((a1 + a2) - (b1 + b2)) < 0.001);
    check("sender and receiver changed by the same amount", fabs((a1 - b1) - (b2 - a2)) < 0.001 && done == 10);

    logout(1, c1);
    logout(1, c2);

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
