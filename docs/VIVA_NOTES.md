# Viva Notes

## 30-second summary

"Our bank is a client-server program in C. The server listens on a TCP socket and creates a new
process with `fork()` for every request. Data is stored in text files with fixed-length records.
When a process wants to change a record it locks exactly that record with `fcntl()`, so two
processes can never change the same balance at the same time. Every write is followed by `fsync()`."

## The two approaches (and why we chose text files)

| | Database (SQLite) | Text files + locking (our choice) |
|---|---|---|
| ACID | provided by the database | we build it ourselves with locks + `fsync` |
| Locking | database does it internally | we use `fcntl()` record locks (what the syllabus asks for) |
| Explaining it | "the library does it" | every step is visible in our own code |

## A note about the schema picture

The first schema picture had small mistakes: Feedback belongs to a **User** (not to Accounts),
Accounts has no `status` column, the **Loans** table was missing, and "INTEER"/"DATETIM" were typos.
Correct relations: User 1—0..1 Account, Account 1—* Transaction, Account 1—* Loan, User 1—* Feedback,
Employee 0..* assigned loans. See `docs/class_diagram.png`.

## Demo script (about 5 minutes)

1. `make reset && make`, then `./server` in terminal 1.
2. Terminal 2: `./client` -> Customer -> login `cust1` / `pass` -> view balance, deposit 500, withdraw, transfer 50 to account 10002, apply for a loan 1000.
3. Terminal 3: login as `cust1` again -> **"already logged in"** (one session per user). Show `data/accounts.txt` in an editor: balances are plain text.
4. Terminal 3: Manager `mgr1` -> assign the loan to employee 201.
5. Terminal 4: Employee `emp1` -> view assigned loans -> approve. Back in terminal 2 check balance and history: `LOAN_CREDIT` appears.
6. Admin `admin1` -> add employee, change a role. Manager -> deactivate cust2; cust2 can no longer log in.
7. **Concurrency:** stop the clients, run `./stress_test` -> `6 passed, 0 failed`.
8. (Optional wow) `gcc -DNO_LOCKS server.c -o server_nolock`, run it, run `./stress_test` again -> FAIL lines. "This is what the locks prevent."

## Likely questions and short answers

**Why fork() for every request?** Each request gets its own process, so a crash in one request does not kill the server, and several clients are served at the same time. The parent only accepts connections. `SIGCHLD` handler + `waitpid(WNOHANG)` removes finished children so they do not become zombies.

**What is a race condition? Show one.** Two processes read balance 100, both add 10, both write 110: one deposit is lost. `stress_test` with `-DNO_LOCKS` shows exactly this.

**What lock do you use?** `fcntl()` with `F_SETLKW` and `F_WRLCK`: an exclusive byte-range (record) lock that *waits* if somebody else holds it. We lock one record for normal operations and the whole file only when appending a new record.

**Why fixed-length records?** Record N is at byte N × size, so we can `pread`/`pwrite` and lock one record directly, without rewriting the whole file.

**Why read the balance again after locking?** Between "find the account" and "get the lock" another process may have changed it. Only the value read *under the lock* is trustworthy.

**How do you avoid deadlock?** A transfer needs two locks. We always lock the lower record number first, so no circular wait is possible.

**Do mandatory or advisory locks apply?** `fcntl` locks are *advisory*: they only work because every part of our server uses them. That is fine here since all access goes through `database.c`.

**Why does each child open the files itself?** `fcntl` locks belong to a process, and closing any descriptor of a file drops that process's locks. Each child has its own descriptors, so its locks are its own.

**How do you enforce one session per user?** `logged_in` flag in `users.txt`. Login changes it from 0 to 1 under the user's record lock, so two simultaneous logins cannot both win. Logout, Exit and Ctrl+C reset it; on server start all flags are cleared (in case of a crash).

**Can a customer cheat by editing the request?** No. The server reads the real role from `users.txt` and ignores the role sent by the client. The command table says which role may call which action.

**Explain ACID with the transfer.** Atomic: both accounts change or neither (undo on failure). Consistent: no negative balance, no money created. Isolated: both accounts locked during the transfer. Durable: `fsync()` after writing.

**What system calls do you use?** `socket, bind, listen, accept, connect, send, recv, fork, waitpid, sigaction, open, close, pread, pwrite, fsync, fstat, fcntl, write` (for the log).

**What would you improve?** Hash passwords, write-ahead log for crash-safe transfers, integer paise instead of `double`, threads or a process pool instead of one process per request.

## Protocol cheat-sheet

`ACTION|role|user_id|arg1|arg2` -> reply `SUCCESS|text` or `ERROR|text`

| Role | Actions |
|---|---|
| any | `CHANGE_PASS` |
| 1 Customer | `VIEW_BALANCE DEPOSIT WITHDRAW TRANSFER VIEW_HISTORY APPLY_LOAN MY_LOANS ADD_FEEDBACK` |
| 2 Employee | `LIST_CUSTOMERS ADD_CUSTOMER MOD_CUSTOMER VIEW_PASSBOOK VIEW_ASSIGNED_LOANS PROCESS_LOAN` |
| 3 Manager | `LIST_CUSTOMERS ACT_DEACT_ACC VIEW_LOANS LIST_EMPLOYEES ASSIGN_LOAN REVIEW_FEEDBACK` |
| 4 Admin | `LIST_USERS ADD_EMPLOYEE MOD_USER_DETAILS MANAGE_ROLES` |
| no session needed | `LOGIN|role|username|password`, `CREATE_CUSTOMER|1|username|password`, `LOGOUT` |
