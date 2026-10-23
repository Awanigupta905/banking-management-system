// database.c
// The "database" is a set of plain text files inside the data/ folder:
//
//     data/users.txt   data/accounts.txt   data/transactions.txt
//     data/loans.txt   data/feedback.txt
//
// Every record is ONE line of FIXED length (padded with spaces), fields are
// separated by '|'. Because every line has the same length, record number N
// always starts at byte N * record_size. That makes record locking easy:
// to lock one record we lock exactly those bytes with fcntl().
//
//     data/accounts.txt  (record size 128)
//     10001|101|5000.00                      <- padded with spaces up to 127 chars + '\n'
//     10002|102|200.00

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    const char *path;
    int rec_size;    // bytes per record, including the final '\n'
    int fd;
} Table;

Table users_t    = {"data/users.txt",        128, -1};
Table accounts_t = {"data/accounts.txt",     128, -1};
Table txns_t     = {"data/transactions.txt", 160, -1};
Table loans_t    = {"data/loans.txt",        128, -1};
Table feedback_t = {"data/feedback.txt",     384, -1};

static Table *all_tables[] = {&users_t, &accounts_t, &txns_t, &loans_t, &feedback_t};
#define NUM_TABLES 5

/* ------------------------------------------------------------------ */
/*  Opening and closing                                                */
/* ------------------------------------------------------------------ */

// Every child process opens the files itself. (fcntl locks belong to a
// process, so each process needs its own descriptors.)
int db_open(void) {
    for (int i = 0; i < NUM_TABLES; i++) {
        all_tables[i]->fd = open(all_tables[i]->path, O_RDWR | O_CREAT, 0666);
        if (all_tables[i]->fd < 0) {
            perror(all_tables[i]->path);
            return -1;
        }
    }
    return 0;
}

void db_close(void) {
    for (int i = 0; i < NUM_TABLES; i++) {
        if (all_tables[i]->fd >= 0) close(all_tables[i]->fd);
        all_tables[i]->fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Record locking (fcntl byte-range locks)                            */
/* ------------------------------------------------------------------ */

// F_SETLKW = "set lock and WAIT": if somebody else has these bytes locked,
// we sleep until they let go. Two processes can never hold the same record.
static void lock_range(int fd, short type, off_t start, off_t len) {
#ifdef NO_LOCKS
    return;                      // demo only: build with -DNO_LOCKS to SEE the race conditions
#endif
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;            // F_WRLCK = exclusive, F_UNLCK = release
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;              // len 0 = "until the end of the file, however long it grows"

    while (fcntl(fd, F_SETLKW, &fl) == -1) {
        if (errno != EINTR) { perror("fcntl"); return; }   // EINTR = signal arrived, retry
    }
}

// lock / unlock ONE record
void rec_lock(Table *t, int idx)   { lock_range(t->fd, F_WRLCK, (off_t)idx * t->rec_size, t->rec_size); }
void rec_unlock(Table *t, int idx) { lock_range(t->fd, F_UNLCK, (off_t)idx * t->rec_size, t->rec_size); }

// lock / unlock the WHOLE file (used when adding a new record at the end)
void table_lock(Table *t)   { lock_range(t->fd, F_WRLCK, 0, 0); }
void table_unlock(Table *t) { lock_range(t->fd, F_UNLCK, 0, 0); }

/* ------------------------------------------------------------------ */
/*  Reading and writing one record                                     */
/* ------------------------------------------------------------------ */

int rec_count(Table *t) {
    struct stat st;
    if (fstat(t->fd, &st) != 0) return 0;
    return (int)(st.st_size / t->rec_size);
}

// Reads record number idx into line (without the padding). Returns 0 on success.
int rec_read(Table *t, int idx, char *line) {
    ssize_t n = pread(t->fd, line, t->rec_size, (off_t)idx * t->rec_size);
    if (n != t->rec_size) return -1;

    line[t->rec_size] = '\0';
    int len = t->rec_size - 1;
    while (len > 0 && (line[len] == ' ' || line[len] == '\n' || line[len] == '\0')) len--;
    line[len + 1] = '\0';
    return 0;
}

// Overwrites (or creates) record number idx. fsync() pushes it to the disk (Durability).
int rec_write(Table *t, int idx, const char *text) {
    char buf[512];
    size_t len = strlen(text);
    if ((int)len > t->rec_size - 1) return -1;

    memset(buf, ' ', t->rec_size);
    memcpy(buf, text, len);
    buf[t->rec_size - 1] = '\n';

    if (pwrite(t->fd, buf, t->rec_size, (off_t)idx * t->rec_size) != t->rec_size) return -1;
    fsync(t->fd);
    return 0;
}

// Splits "a|b|c" in place. Returns the number of fields.
static int split(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        p = strchr(p, '|');
        if (!p) break;
        *p++ = '\0';
    }
    return n;
}

static double round2(double x) {
    return (long long)(x * 100 + (x >= 0 ? 0.5 : -0.5)) / 100.0;
}

static void now_string(char *buf, size_t size) {
    time_t now = time(NULL);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

/* ------------------------------------------------------------------ */
/*  The five record types                                              */
/* ------------------------------------------------------------------ */

typedef struct { int id; char name[32]; char pass[32]; int role; int active; int logged_in; } User;
typedef struct { int id; int user_id; double balance; } Account;
typedef struct { int id; int account_id; char type[16]; double amount; char details[48]; char time[24]; } Txn;
typedef struct { int id; int account_id; double amount; char status[12]; int emp_id; } Loan;
typedef struct { int id; int user_id; char time[24]; char msg[300]; } Feedback;

/* ---- users:  id|username|password|role|active|logged_in ---- */

int user_get(int idx, User *u) {
    char line[512], *f[6];
    if (rec_read(&users_t, idx, line) != 0 || split(line, f, 6) != 6) return -1;
    u->id = atoi(f[0]);
    snprintf(u->name, sizeof u->name, "%s", f[1]);
    snprintf(u->pass, sizeof u->pass, "%s", f[2]);
    u->role = atoi(f[3]);
    u->active = atoi(f[4]);
    u->logged_in = atoi(f[5]);
    return 0;
}

int user_put(int idx, const User *u) {
    char line[256];
    snprintf(line, sizeof line, "%d|%s|%s|%d|%d|%d", u->id, u->name, u->pass, u->role, u->active, u->logged_in);
    return rec_write(&users_t, idx, line);
}

// Returns the record number of a user id (or -1). Plain linear search, the files are small.
int user_find(int uid, User *u) {
    int n = rec_count(&users_t);
    for (int i = 0; i < n; i++) {
        if (user_get(i, u) == 0 && u->id == uid) return i;
    }
    return -1;
}

int user_find_name(const char *name, User *u) {
    int n = rec_count(&users_t);
    for (int i = 0; i < n; i++) {
        if (user_get(i, u) == 0 && strcmp(u->name, name) == 0) return i;
    }
    return -1;
}

/* ---- accounts:  account_id|user_id|balance ---- */

int account_get(int idx, Account *a) {
    char line[512], *f[3];
    if (rec_read(&accounts_t, idx, line) != 0 || split(line, f, 3) != 3) return -1;
    a->id = atoi(f[0]);
    a->user_id = atoi(f[1]);
    a->balance = atof(f[2]);
    return 0;
}

int account_put(int idx, const Account *a) {
    char line[256];
    snprintf(line, sizeof line, "%d|%d|%.2f", a->id, a->user_id, a->balance);
    return rec_write(&accounts_t, idx, line);
}

int account_find_id(int account_id, Account *a) {
    int n = rec_count(&accounts_t);
    for (int i = 0; i < n; i++) {
        if (account_get(i, a) == 0 && a->id == account_id) return i;
    }
    return -1;
}

int account_find_user(int uid, Account *a) {
    int n = rec_count(&accounts_t);
    for (int i = 0; i < n; i++) {
        if (account_get(i, a) == 0 && a->user_id == uid) return i;
    }
    return -1;
}

/* ---- transactions:  txn_id|account_id|type|amount|details|time ---- */

int txn_get(int idx, Txn *t) {
    char line[512], *f[6];
    if (rec_read(&txns_t, idx, line) != 0 || split(line, f, 6) != 6) return -1;
    t->id = atoi(f[0]);
    t->account_id = atoi(f[1]);
    snprintf(t->type, sizeof t->type, "%s", f[2]);
    t->amount = atof(f[3]);
    snprintf(t->details, sizeof t->details, "%s", f[4]);
    snprintf(t->time, sizeof t->time, "%s", f[5]);
    return 0;
}

// Adds one line to the passbook (end of transactions.txt)
void txn_add(int account_id, const char *type, double amount, const char *details) {
    char line[256], when[24];
    now_string(when, sizeof when);

    table_lock(&txns_t);
    int idx = rec_count(&txns_t);
    snprintf(line, sizeof line, "%d|%d|%s|%.2f|%s|%s", idx + 1, account_id, type, amount, details, when);
    rec_write(&txns_t, idx, line);
    table_unlock(&txns_t);
}

/* ---- loans:  loan_id|account_id|amount|status|employee_id ---- */

int loan_get(int idx, Loan *l) {
    char line[512], *f[5];
    if (rec_read(&loans_t, idx, line) != 0 || split(line, f, 5) != 5) return -1;
    l->id = atoi(f[0]);
    l->account_id = atoi(f[1]);
    l->amount = atof(f[2]);
    snprintf(l->status, sizeof l->status, "%s", f[3]);
    l->emp_id = atoi(f[4]);
    return 0;
}

int loan_put(int idx, const Loan *l) {
    char line[256];
    snprintf(line, sizeof line, "%d|%d|%.2f|%s|%d", l->id, l->account_id, l->amount, l->status, l->emp_id);
    return rec_write(&loans_t, idx, line);
}

// Loan ids are 1, 2, 3... so loan N is record N-1
int loan_find(int loan_id, Loan *l) {
    if (loan_id < 1 || loan_id > rec_count(&loans_t)) return -1;
    if (loan_get(loan_id - 1, l) != 0 || l->id != loan_id) return -1;
    return loan_id - 1;
}

/* ---- feedback:  id|user_id|time|message ---- */

int feedback_get(int idx, Feedback *fb) {
    char line[512], *f[4];
    if (rec_read(&feedback_t, idx, line) != 0 || split(line, f, 4) != 4) return -1;
    fb->id = atoi(f[0]);
    fb->user_id = atoi(f[1]);
    snprintf(fb->time, sizeof fb->time, "%s", f[2]);
    snprintf(fb->msg, sizeof fb->msg, "%s", f[3]);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Creating the files (done once, by the parent server)               */
/* ------------------------------------------------------------------ */

static void seed_user(int id, const char *name, const char *pass, int role) {
    User u = {id, "", "", role, 1, 0};
    snprintf(u.name, sizeof u.name, "%s", name);
    snprintf(u.pass, sizeof u.pass, "%s", pass);
    user_put(rec_count(&users_t), &u);
}

static void seed_account(int id, int uid, double balance) {
    Account a = {id, uid, balance};
    account_put(rec_count(&accounts_t), &a);
}

int db_init(void) {
    mkdir("data", 0755);
    if (db_open() != 0) return -1;

    if (rec_count(&users_t) == 0) {            // first start: sample data
        seed_user(101, "cust1", "pass", 1);
        seed_user(102, "cust2", "pass2", 1);
        seed_user(201, "emp1", "pass", 2);
        seed_user(301, "mgr1", "pass", 3);
        seed_user(401, "admin1", "pass", 4);
        seed_account(10001, 101, 5000.00);
        seed_account(10002, 102, 200.00);
    }

    // if the server crashed earlier, nobody can really be logged in now
    User u;
    for (int i = 0; i < rec_count(&users_t); i++) {
        if (user_get(i, &u) == 0 && u.logged_in) {
            u.logged_in = 0;
            user_put(i, &u);
        }
    }

    db_close();
    printf("DB: text files ready in data/, sample users loaded, old sessions cleared.\n");
    return 0;
}
