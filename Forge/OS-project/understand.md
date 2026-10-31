# Complete Project Understanding & OS Concepts Guide

## 1. Project Identity

- **Team Name:** task killers  
- **Project Title:** Linux Service and Process Supervisor  
- **Course:** Operating Systems  
- **Implementation Language:** C11 using POSIX/Linux system calls & Modern Web UI  
- **Repository:** https://github.com/slashnight/KLH_CSE_2026-27_Team-22_Service-and-process-supervisor  

This project is a lightweight, high-reliability service supervisor and process orchestrator. It behaves like a mini `systemd` or `supervisord`: it parses declarative service definitions, starts child processes, tracks their state, detects crashes and hangs, restarts failed services with exponential backoff, accepts process control signals, maintains unbuffered audit trails, and exposes real-time telemetry through both a terminal CLI and an embedded Web Console.

The supervisor is intended to run on Linux or Ubuntu through WSL. The Windows `run.bat` or `npm run dev` script acts as a cross-platform bridge; the supervisor core executes in POSIX/Linux using system APIs such as `fork()`, `execvp()`, `setsid()`, `waitpid()`, `pipe()`, `killpg()`, `sigaction()`, `fcntl()`, `getrusage()`, and `select()`.

---

## 2. Real-World Scenario & Motivation

In production cloud and on-premise Linux environments, hundreds of background processes run concurrently:
- Web and API servers (NGINX, Node.js, Go)
- Database services (PostgreSQL, Redis)
- Telemetry & metric collectors (Prometheus node-exporter)
- Logging and audit daemons
- Background task queues (Celery, sidekiq)

These processes inevitably fail due to segmentation faults, out-of-memory errors, uncaught exceptions, deadlocks, or network timeouts. System administrators cannot manually monitor and restart crashed services 24/7. 

This project solves that problem by acting as the **parent supervisor (PID 1 of the service tree)**:
1. It maintains an authoritative in-memory registry of all managed processes.
2. It detects process termination instantaneously using asynchronous kernel signals (`SIGCHLD`) and non-blocking reaping (`waitpid`).
3. It detects silent hangs and deadlocks using anonymous pipe heartbeats (`pipe()`).
4. It isolates child process groups (`setsid`) so rogue child threads and grandchildren can be terminated cleanly.
5. It offers full remote observability and control via an embedded HTTP server and web dashboard.

---

## 3. Core Operating System Concepts & API Implementation

### 3.1 Process Creation: `fork()` and `execvp()`

The supervisor uses the classic POSIX two-step process creation model:

```c
pid_t pid = fork();
if (pid < 0) {
    /* fork failed: log error and return */
} else if (pid == 0) {
    /* CHILD EXECUTION SPACE */
    setsid(); /* create new session and process group */
    
    /* Redirect stdout/stderr to logs/<name>.out */
    int out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    dup2(out_fd, STDOUT_FILENO);
    dup2(out_fd, STDERR_FILENO);
    close(out_fd);

    /* Bind heartbeat pipe write end to fd 3 */
    if (svc->heartbeat_enabled) {
        dup2(svc->hb_pipe[1], SUPERVISOR_HB_FD);
        close(svc->hb_pipe[0]);
        close(svc->hb_pipe[1]);
    }

    execvp(svc->argv[0], svc->argv);
    _exit(127); /* reached only if execvp fails */
} else {
    /* PARENT EXECUTION SPACE */
    svc->pid = pid;
    svc->pgid = pid;
    svc->state = STATE_RUNNING;
    /* close pipe write end in parent */
    if (svc->heartbeat_enabled) {
        close(svc->hb_pipe[1]);
    }
}
```

- **`fork()`**: Clones the parent process address space. The child gets a duplicate copy of file descriptors and memory pages (with Copy-on-Write optimization).
- **`setsid()`**: Detaches the child from the parent's controlling terminal and creates a new process group where `PGID == PID`. This ensures signals sent to the child's process group (`killpg`) terminate all sub-children without affecting the supervisor parent.
- **`dup2()`**: Duplicates file descriptors. Standard output (`STDOUT_FILENO = 1`) and standard error (`STDERR_FILENO = 2`) are redirected to `logs/<name>.out`.
- **`execvp()`**: Overwrites the child's address space, stack, heap, and text segment with the new worker binary, searching `PATH` if necessary.

---

### 3.2 Non-Blocking Process Reaping: `waitpid(..., WNOHANG)`

When a child process terminates, the Linux kernel removes its memory but retains its entry in the process table (exit status, PID, resource usage). Until the parent reads this status, the process is a **zombie**.

If a supervisor used blocking `wait()` or `waitpid()`, the entire event loop, CLI, and HTTP server would freeze until a child died. Instead, this project uses:

```c
int status;
pid_t pid;
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    handle_child_exit(pid, status);
}
```

- **`-1`**: Reaps any child belonging to this supervisor.
- **`WNOHANG`**: Returns `0` immediately if children are still running, preventing blocking.
- **Kernel Exit Analysis**:
  - `WIFEXITED(status)`: Evaluates true if child called `exit()` or returned from `main()`. `WEXITSTATUS(status)` extracts the numerical exit code.
  - `WIFSIGNALED(status)`: Evaluates true if child was terminated by an unhandled signal (e.g. `SIGSEGV`, `SIGKILL`). `WTERMSIG(status)` extracts the fatal signal number.

---

### 3.3 Inter-Process Communication: Anonymous Pipe Heartbeats

While `waitpid()` detects when a process terminates, it **cannot detect when a process hangs, deadlocks, or gets stuck in an infinite loop**. The process is still alive (`RUNNING`), but functionally dead.

To solve this, the supervisor implements **IPC Heartbeats**:

```text
Supervisor Parent (select loop)                Child Worker Process
-------------------------------                --------------------
pipe(pipefd)                                   Inherited fd 3 (SUPERVISOR_HB_FD)
Close pipefd[1] (write end)                    Close pipefd[0] (read end)
select() monitors pipefd[0]                    write(3, "H", 1) every 2 seconds
drain_heartbeats() reads 'H' ----------------> Continues execution
Updates svc->last_heartbeat = now
```

1. **Watchdog Evaluation (`check_health()`)**:
   Every 1 second, the supervisor checks:
   `if (now - svc->last_heartbeat >= svc->heartbeat_timeout_sec)`
2. If exceeded, the worker is marked `STATE_UNHEALTHY`, an audit warning is logged, and the supervisor delivers `SIGTERM` (escalating to `SIGKILL`) to force recovery.
3. The Web Console visualizes this in real time with an animated glowing dot (`@keyframes hbPulseAnim`), showing countdowns such as `Pulse (1s ago) / 6s max`.

---

### 3.4 Signal Handling: `sigaction()`

POSIX signals are asynchronous software interrupts. Doing complex work (like `printf`, `malloc`, or file I/O) inside a signal handler causes deadlocks because those functions are not async-signal-safe.

This project implements the **Self-Pipe / Atomic Flag Pattern**:

```c
static volatile sig_atomic_t g_child_exited = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_reload_requested = 0;

static void handle_sigchld(int sig) {
    (void)sig;
    g_child_exited = 1;
}
```

- Handlers are installed via `sigaction()` with `SA_RESTART` (restarts interrupted system calls) and `SA_NOCLDSTOP` (ignores stop/continue signals).
- Handlers set a `volatile sig_atomic_t` flag (guaranteed atomic read/write by hardware).
- The central `select()` event loop checks these flags safely during its normal execution flow.

---

### 3.5 I/O Multiplexing: `select()` Event Loop

The supervisor needs to watch multiple I/O streams simultaneously:
1. User input from terminal `stdin` (`STDIN_FILENO`).
2. Incoming TCP connections on the HTTP listen socket (`g_http_listen_fd`).
3. HTTP client request buffers across active client sockets.
4. Heartbeat pulse pipes across all active worker processes.

Instead of spawning multi-threaded complexity or polling in a tight CPU-wasting loop, the supervisor uses POSIX `select()`:

```c
fd_set read_fds;
FD_ZERO(&read_fds);
FD_SET(STDIN_FILENO, &read_fds);
FD_SET(g_http_listen_fd, &read_fds);

for (int i = 0; i < g_supervisor.count; i++) {
    if (g_supervisor.services[i].hb_pipe[0] > 0) {
        FD_SET(g_supervisor.services[i].hb_pipe[0], &read_fds);
    }
}

struct timeval timeout = { .tv_sec = 0, .tv_usec = 250000 };
int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
```

The kernel puts the supervisor process to sleep until either:
- Any file descriptor becomes readable, OR
- The 250ms timeout expires (allowing fast periodic watchdog, child reaping, and restart timer checks).

---

### 3.6 Low-Level Unbuffered Audit Logging: `open()`, `write()`, `close()`

Standard C library functions (`fprintf`, `fputs`) use userspace buffers (`BUFSIZ`, typically 4096 or 8192 bytes). If a process crashes or experiences a sudden power loss, unbuffered data remaining in memory is lost.

For security and audit reliability, `src/logger.c` writes directly to disk:

```c
int fd = open("logs/supervisor.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
write(fd, buffer, length);
```

- `O_APPEND`: Guarantees atomic writes at the end of the file, even if multiple threads or processes append.
- Standard format: `[YYYY-MM-DD HH:MM:SS] [LEVEL] [PID: xxx] message`.

---

### 3.7 Resource Accounting: `getrusage()`

The kernel tracks detailed CPU and memory statistics for processes and their children. The supervisor queries these via:

```c
struct rusage ru;
getrusage(RUSAGE_CHILDREN, &ru);
```

- `ru.ru_utime`: Cumulative CPU time spent in user space.
- `ru.ru_stime`: Cumulative CPU time spent in kernel/system space.
- `ru.ru_maxrss`: Maximum resident set size in kilobytes (peak physical memory).
- Combined with `/proc/[pid]/statm` for supervisor resident memory footprint.

---

### 3.8 Embedded HTTP REST API Server (`src/http_server.c`)

The supervisor embeds a non-blocking HTTP 1.1 server directly inside the supervisor process:
- Listens on `0.0.0.0:8080`.
- Socket set to non-blocking via `fcntl(fd, F_SETFL, O_NONBLOCK)`.
- Serves static assets (`index.html`, `styles.css`, `app.js`) with cache prevention:
  ```http
  HTTP/1.1 200 OK
  Content-Type: text/html; charset=utf-8
  Cache-Control: no-cache, no-store, must-revalidate
  Pragma: no-cache
  Expires: 0
  ```
- Exposes JSON endpoints:
  - `GET /api/status`: Complete JSON snapshot of all services, PIDs, memory, CPU, and heartbeat pulse timers.
  - `POST /api/action`: Executes signals and lifecycle actions.
  - `GET /api/logs?service=<name>`: Streams audit logs or worker output files.
  - `GET /api/config` & `POST /api/config`: Reads or updates `config/services.conf` with instant `SIGHUP` hot reload.

---

### 3.9 Single-Instance File Locking: `fcntl(F_SETLK)`

To guarantee that multiple supervisor daemons cannot run simultaneously and fight over child processes or port 8080:

```c
int lock_fd = open("logs/supervisor.lock", O_RDWR | O_CREAT, 0644);
struct flock fl = {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0
};
if (fcntl(lock_fd, F_SETLK, &fl) < 0) {
    /* Another supervisor instance is already running */
    exit(1);
}
```

The kernel automatically releases POSIX record locks if the process terminates, guaranteeing clean recovery.

---

## 4. Source File Walkthrough

### `include/supervisor.h`
Data definitions, `service_t` (PIDs, PGIDs, restart counters, heartbeat descriptors, state enums), `supervisor_t`, and shared macros (`SUPERVISOR_HB_FD = 3`).

### `src/supervisor.c`
Supervisor entry point. Parses command line, acquires instance lock, initializes logging, sets up signals, loads config, starts services, runs the `select()` event loop, handles CLI commands, and coordinates shutdown.

### `src/config.c`
Reads `config/services.conf`. Splits strings into `argv` arrays, validates directives (`command`, `autorestart`, `heartbeat`, `depends_on`), and handles dynamic reload on `SIGHUP`.

### `src/process.c`
Process management engine. Executes `fork()` and `execvp()`, handles output redirection, manages anonymous pipes, reaps children with `waitpid(..., WNOHANG)`, delivers process group signals with `killpg()`, and schedules restarts.

### `src/signals.c`
Registers signal handlers with `sigaction()`. Captures `SIGCHLD`, `SIGINT`, `SIGTERM`, `SIGHUP` and converts them into atomic flags.

### `src/logger.c`
Audit trail engine. Formats log lines with timestamps and writes directly to disk with unbuffered `open()`, `write()`, and `close()`. Records child resource usage with `getrusage()`.

### `src/http_server.c`
Embedded HTTP server. Listens on port 8080, serves web assets, parses requests, and exposes REST API endpoints for remote status, actions, logs, and configuration.

### `linux-supervisor-website/`
Frontend Web Console. Vanilla JS (`app.js`), CSS3 (`styles.css`), and HTML5 (`index.html`). Connects to `/api/status`, renders live pulsing heartbeat badges, displays process telemetry, and triggers process control actions.

### `dev.js`
Node.js launcher. Verifies if port 8080 is online, boots WSL Ubuntu if offline, and opens the default browser on Windows via `start "" "http://localhost:8080/"`.

---

## 5. Demonstration & Evaluation Walkthrough

### Step 1: Start the Supervisor
Run from project root:
```bash
npm run dev
```
or inside WSL:
```bash
./run.sh
```

### Step 2: Observe Terminal & Web Console
Visit **http://localhost:8080/** in any browser:
- **Dashboard**: All 6 services (`heartbeat`, `logger`, `metrics`, `audit`, `flaky`, `hung`) are displayed.
- **Process Table**: Shows PID, PGID, Status, Restarts/Crashes, Memory, and the new **Heartbeat Pulse** column.
- **Live Heartbeats**: Active workers display glowing green pulse badges: `Pulse (1s ago) / 6s max`.
- **IPC Telemetry Monitor**: Displays status cards for each service detailing pipe(3) IPC status and watchdog timeouts.

### Step 3: Test Multi-Service Fault Recovery & Random Restarts
- **Equal Fault Recovery**: Every worker (`dummy_worker`, `flaky_worker`, `hung_worker`) simulates realistic lifespans (~20s–44s) by seeding PRNG uniquely per instance and exiting with code 1.
- The supervisor catches each exit via `waitpid()`, logs `[WARN] Service '<name>' exited/crashed`, increments permanent `total_restarts`, maintains concurrency across all other services, and restarts the worker automatically with backoff.
- **Random Chaos Restart**: Click `🎲 Random Restart` on the Services page or send `POST /api/action {"action":"random_restart"}` to pick a random running service and trigger an instant controlled restart.
- **Auto-Chaos Mode**: Toggle `⚡ Auto-Chaos: ACTIVE (12s)` in the Web Console toolbar to trigger periodic random restarts every 12 seconds, demonstrating continuous resilience and real-time dashboard updates.

### Step 4: Test Process Signals
In the Web Console, click:
- **Pause**: Sends `SIGSTOP`. Process state transitions to `PAUSED`.
- **Stop**: Sends `SIGTERM`. Process stops cleanly and remains stopped until manually restarted.
- **Stop All**: Click `⏹ Stop All` in the dashboard or services toolbar to terminate all child worker processes while keeping the HTTP supervisor server online and responsive.
- **Start All**: Click `▶ Start All` to restart all eligible services.

### Step 5: Automated Verification Suite
Run the comprehensive test script using npm or python:
```bash
npm test
# or
npm run test_all.py
# or inside WSL Ubuntu:
python3 test_all.py
```
Validates all 9 core capabilities: status querying, random chaos restarts, signal dispatching (`SIGSTOP`/`SIGCONT`), stop/start cycles, configuration inspection, audit logging, multi-service concurrency preservation, and real Linux `/proc` kernel verification (`PPid`, `PGID == PID`, pidfiles, external `SIGKILL` recovery, `SIGHUP` hot reload, and zero zombie processes).

### Step 6: Test Graceful Shutdown
In the terminal, type `quit`, click "Shutdown Backend" on the dashboard, or press `Ctrl+C`:
1. Supervisor logs shutdown request.
2. Sends `SIGTERM` to all active child process groups.
3. Reaps all children cleanly with `waitpid()`.
4. Closes all file descriptors and releases `logs/supervisor.lock`.
5. Exits cleanly with status 0. Zero zombie processes remaining.
*(Note: If running under `npm run dev` / `dev.js`, the dev launcher detects the offline state and offers automated backend recovery).*
