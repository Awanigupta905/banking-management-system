# Banking Management System (CS-513 Mini Project)

A client-server banking system in C. Four roles (Customer, Bank Employee, Manager, Administrator)
talk to one server over TCP sockets. All data is kept in **plain text files** and protected with
**fcntl() record locking**.

## Why text files instead of a database?

The project asks for *file management and locking mechanisms*. With text files of fixed-length
records, record number `N` always starts at byte `N * record_size`, so locking one record is just
locking those bytes with `fcntl()`. Everything is visible and explainable: you can open
`data/accounts.txt` in an editor during the demo and see the balances change.

## Files in this zip

| File | What it is |
|---|---|
| `server.c` | Server: sockets, `fork()`, request handling for all four roles |
| `database.c` | File layer: text-file records, `fcntl()` record locks (included by `server.c`) |
| `client.c` | Menu program for all four roles |
| `stress_test.c` | Race-condition demo: many processes hit the server at the same time |
| `Makefile` | `make`, `make reset`, `make clean` |
| `docs/architecture.png` | Architecture diagram |
| `docs/class_diagram.png` | Class / structure diagram (records, file layer, command table) |
| `docs/roles.png` | What each role can do |
| `docs/VIVA_NOTES.md` | Short explanations and likely viva questions |
| `docs/*.dot` | Source of the diagrams (Graphviz) |

## Build and run

Needs only `gcc` (no extra libraries).

```
make                   # builds server, client, stress_test
./server               # terminal 1  (creates the data/ folder with sample users)
./client               # terminal 2, 3, ...  (as many clients as you like)
```

`make reset` deletes `data/` and `bank.log` so you start again with the sample data.

### Sample users

| Role | Username | Password | Notes |
|---|---|---|---|
| Customer | `cust1` | `pass` | account 10001, balance 5000.00 |
| Customer | `cust2` | `pass2` | account 10002, balance 200.00 |
| Bank Employee | `emp1` | `pass` | user id 201 |
| Manager | `mgr1` | `pass` | user id 301 |
| Administrator | `admin1` | `pass` | user id 401 |

New customers can also register themselves from the customer login screen.

## Features

**Customer:** one-session login, view balance, deposit, withdraw, transfer, apply for loan,
view my loan applications, change password, transaction history, feedback, logout, exit.

**Bank Employee:** add customer (with opening balance), modify customer (password reset),
view passbook of any customer, view assigned loans, approve/reject assigned loans, change password.

**Manager:** activate/deactivate customer accounts, assign loans to employees, review feedback,
change password.

**Administrator:** add bank employee, reset password of any user, manage roles, change password.

Extras added on top of the requirements: lists of customers/employees/users are shown before you
have to type an id; a customer who quits or presses Ctrl+C is logged out automatically; the server
clears stale sessions on start; every request is written to `bank.log`.

## How it works (short)

1. `client` sends one line, e.g. `TRANSFER|1|101|10002|50` = `ACTION|role|user_id|arguments`.
2. `server` accepts the connection and `fork()`s a child process. The child serves that one request.
3. The child checks: is the user logged in? does the role stored in the file match? is the action
   allowed for this role? Then it calls the handler from a command table.
4. The handler locks the record(s) it needs with `fcntl(F_SETLKW)`, reads them again, changes
   them, writes them back (`pwrite` + `fsync`) and unlocks.
5. The child sends `SUCCESS|...` or `ERROR|...` and exits.

## Concurrency and synchronization

* **Record locking:** `rec_lock()` locks exactly the bytes of one record. A second process that
  wants the same record sleeps (`F_SETLKW`) until the first one is done. Processes working on
  *different* accounts do not block each other.
* **Read again after locking:** every handler re-reads the record *after* it holds the lock, so
  it never works with a stale balance (this prevents the classic lost update).
* **No deadlock in transfers:** a transfer locks two accounts, always the lower record number
  first, so two opposite transfers cannot wait for each other forever.
* **One session per user:** login flips `logged_in` from 0 to 1 while holding the user's record
  lock, so two simultaneous logins cannot both succeed.
* **New records** (new user, loan, transaction) are appended while the whole file is locked, so
  two processes never get the same id.

Run `./server` in one terminal and `./stress_test` in another. It fires 5 simultaneous logins,
20 simultaneous deposits, 10 simultaneous withdrawals and 10 simultaneous transfers and checks
that no money is lost, created or spent twice. Expected result: `6 passed, 0 failed`.

To *see* what the locks protect: build the server without them
(`gcc -DNO_LOCKS server.c -o server_nolock`), run the same stress test, and watch it fail
(deposits get lost, money disappears in transfers).

## ACID properties

| Property | How it is achieved |
|---|---|
| **Atomicity** | A transfer changes two accounts. Both are locked first; if the second write fails, the first one is written back to its old value. A withdrawal checks and subtracts under one lock, so it either fully happens or not at all. A loan approval marks the loan, pays the money, and un-marks the loan if paying fails. |
| **Consistency** | Balances never go below 0 (checked under the lock). Amounts are validated (positive, max 10,000,000, 2 decimals). Roles are checked against the file, usernames are unique, a loan can only be processed by the employee it is assigned to, and only once. |
| **Isolation** | Record locks make concurrent requests behave as if they ran one after the other. |
| **Durability** | Every write is followed by `fsync()`, so a completed operation survives a crash or power cut. |

## Known limitations (honest list)

* Passwords are stored as plain text to keep the code simple. A real bank would store a hash.
* Atomicity of the two-account transfer is implemented by hand (write back the old value). If the
  server process is killed *exactly* between the two writes, money could be lost. A real system
  would use a write-ahead log.
* Balances are `double`; a real bank would store whole paise as integers.
* Every request opens a new TCP connection (simple, but not the fastest design).
* Searching a record by id is a linear scan, which is fine for a project-sized data set.
