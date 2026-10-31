# Project Practice & Presentation Defense Guide

## 1. Project Overview

- **Project Title:** Linux Service and Process Supervisor  
- **Team Name:** task killers  
- **Course:** Operating Systems  
- **Implementation Language:** C11 with POSIX/Linux system calls & Modern Web Console  
- **Repository:** https://github.com/slashnight/KLH_CSE_2026-27_Team-22_Service-and-process-supervisor  

This guide provides the exact demonstration scripts, viva defense answers, architecture explanations, and technical questions required to present and defend the project with distinction.

---

## 2. The 90-Second Elevator Pitch

> "Our project is an OS-level Linux service and process supervisor written in C11. It manages background processes similarly to systemd or supervisord. It reads declarative service definitions, spawns isolated child processes using `fork()` and `execvp()`, isolates process groups with `setsid()`, and continuously reaps terminating children using non-blocking `waitpid(..., WNOHANG)`. 
>
> If a worker crashes, the supervisor detects the exit status, increments crash counters, and automatically restarts it with exponential backoff delay. To detect hung or deadlocked processes that remain alive but unresponsive, we implemented an Inter-Process Communication (IPC) heartbeat system using anonymous pipes: workers inherit file descriptor 3 and transmit pulses every 2 seconds. If a worker stops beating within its watchdog timeout, the supervisor marks it `UNHEALTHY` and triggers automated signal recovery via `killpg`.
>
> Furthermore, the C supervisor embeds a lightweight, non-blocking HTTP REST API server on port 8080 directly multiplexed into its POSIX `select()` event loop. This powers a real-time web dashboard featuring live pulsating heartbeat animations, process signal buttons (`SIGSTOP`, `SIGCONT`, `SIGTERM`, `SIGKILL`), an IPC telemetry monitor, and live audit log streaming."

---

## 3. Team Member Viva Responsibilities

### Sai Kiran (2520030125) — Service Management & Configuration Engine
- **What to explain:**
  - Parsing `config/services.conf` in `src/config.c` and building null-terminated `argv` arrays for `execvp()`.
  - Process creation flow in `src/process.c`: `fork()`, error checking, child initialization.
  - Process group isolation via `setsid()` to ensure sub-children can be cleanly signaled.
  - Dynamic configuration reloading via `SIGHUP` and `/api/config`.
- **Key System Calls:** `fork()`, `execvp()`, `setsid()`, `chdir()`, `getpid()`, `getppid()`.

### Harshith (2520030393) — Monitoring, Signal Handling & Fault Recovery
- **What to explain:**
  - Safe asynchronous signal handling in `src/signals.c` using `sigaction()` and `volatile sig_atomic_t` flags.
  - Non-blocking child reaping via `waitpid(-1, &status, WNOHANG)` to completely prevent zombie processes.
  - Status decoding macros: `WIFEXITED()`, `WEXITSTATUS()`, `WIFSIGNALED()`, `WTERMSIG()`.
  - Process control signals (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`) sent via `killpg()`.
  - Graceful multi-step shutdown coordination (`SIGTERM` -> timeout -> `SIGKILL`).
- **Key System Calls:** `sigaction()`, `waitpid()`, `killpg()`.

### Nishanth (2520030323) — Resource Accounting, Audit Logging & IPC Interface
- **What to explain:**
  - Unbuffered audit logging in `src/logger.c` using direct POSIX system calls (`open()`, `write()`, `close()`) to prevent data loss on sudden crashes.
  - Resource usage accounting via `getrusage(RUSAGE_CHILDREN, ...)`.
  - Anonymous pipe heartbeat telemetry: `pipe()` allocation, binding write end to descriptor 3 (`SUPERVISOR_HB_FD`), and non-blocking draining inside the `select()` event loop.
  - Embedded HTTP REST API in `src/http_server.c` multiplexed inside `select()`.
- **Key System Calls:** `open()`, `write()`, `close()`, `pipe()`, `dup2()`, `getrusage()`, `select()`, `socket()`.

---

## 4. Step-by-Step Technical Walkthrough

### Step 1: Single-Instance Mutual Exclusion (`fcntl`)
At startup, `supervisor.c` opens `logs/supervisor.lock` and acquires a non-blocking write lock via `fcntl(F_SETLK)`. If another supervisor is already running, it immediately warns the user and exits, preventing port 8080 conflicts and duplicate child processes.

### Step 2: Unbuffered Logging Initialization
`logger.c` opens `logs/supervisor.log` with `open(path, O_WRONLY | O_CREAT | O_APPEND, 0644)`. Every entry contains `[YYYY-MM-DD HH:MM:SS] [LEVEL] [PID: xxx] message` written atomically using POSIX `write()`.

### Step 3: Declarative Configuration & Dependency Resolution
`config.c` reads `config/services.conf`. Services configured with `depends_on` (e.g. `logger` depends on `heartbeat`, and `audit` depends on `logger`) start in `STATE_WAITING` and automatically transition to `STATE_STARTING` when prerequisites become stable.

### Step 4: Process Spawning (`fork` + `setsid` + `execvp`)
For each service:
1. `pipe()` creates an anonymous pipe channel.
2. `fork()` creates the child process.
3. In child: `setsid()` creates a new session/process group; `dup2()` redirects stdout/stderr to `logs/<name>.out`; write end of pipe is duplicated to fd 3 (`SUPERVISOR_HB_FD`); `execvp()` replaces the memory image.
4. In parent: Child PID/PGID is registered; read end of pipe is registered with `select()`.

### Step 5: Multiplexed Event Loop (`select`)
The supervisor never blocks on one task. The `select()` system call watches:
- `STDIN_FILENO` for interactive terminal commands.
- `g_http_listen_fd` and active HTTP client sockets for web traffic.
- Worker heartbeat pipe read descriptors.
- A 1-second timeout for periodic health and restart timer evaluation.

### Step 6: Heartbeat Telemetry & Watchdog Recovery
- Active workers write `'H'` every 2 seconds to fd 3.
- `drain_heartbeats()` reads available bytes non-blockingly, updating `svc->last_heartbeat`.
- `check_health()` checks if `(now - last_heartbeat) >= timeout`.
- If a worker hangs (like `hung_worker`), it is marked `UNHEALTHY`, signaled with `SIGTERM`/`SIGKILL`, and restarted.
- The web console shows glowing green pulsating indicators (`Pulse (1s ago) / 6s max`).

### Step 7: Crash Detection & Exponential Backoff
- When `flaky_worker` exits with code 1, the kernel issues `SIGCHLD`.
- The supervisor reaps the exit via `waitpid(..., WNOHANG)`.
- It records `crash_count++`, logs the failure, and computes the backoff: `delay * 2^(attempts-1)`.
- When the timer expires, it re-spawns the worker cleanly.

---

## 5. Live Demonstration Script

### Option A: Web Console Demonstration (Recommended)
From PowerShell or terminal:
```bash
npm run dev
```
1. Point out the terminal log: `✓ C Supervisor backend is ALREADY running and online at http://localhost:8080/`.
2. Browser opens automatically at **http://localhost:8080/**.
3. **Dashboard Tab**:
   - Point out **Managed services: 6** and **Running services: 6**.
   - Point out the **Service Overview Table** with the **Heartbeat** column showing live pulses.
   - Click **⏹ Stop All**: demonstrate that all services stop safely via `SIGTERM` while the C supervisor and web console stay connected.
   - Click **▶ Start All**: show instant concurrent recovery of all services.
4. **Services Tab**:
   - Point out the **Process Table**: explain kernel PID, PGID, memory (`rss_kb`), and the animated pulsing green badges (`Pulse (1s ago) / 6s max`).
   - Point out the **IPC Anonymous Pipe Heartbeat Telemetry** cards: explain how child processes inherit pipe fd 3 and transmit pulses.
   - Click **Pause** on `heartbeat`: verify state changes to `PAUSED` via `SIGSTOP`.
   - Click **Resume**: verify state transitions back to `RUNNING` via `SIGCONT`.
   - Click **Restart** on `metrics`: verify process is cleanly stopped and respawned.
   - Click **🎲 Random Restart**: show random chaos fault injection.
   - Toggle **⚡ Auto-Chaos: ACTIVE (12s)**: demonstrate continuous background resilience.
5. **Logs Tab**:
   - Select `heartbeat service (logs/heartbeat.out)` from the dropdown.
   - Show the continuous stream of `[heartbeat] heartbeat 350 at...` lines highlighted in green.
6. **Automated Test Suite**:
   - Run from terminal:
     ```bash
     npm test
     # or
     npm run test_all.py
     ```
   - Show all 9 test suites passing cleanly with green checkmarks (including Linux `/proc` `PPid`/`PGID` verification, external `kill -9` recovery, `SIGHUP` reload, and zero-zombie verification).

### Option B: Terminal CLI Demonstration
Inside Ubuntu WSL:
```bash
./bin/supervisor -c config/services.conf
```
Type:
1. `status` — Displays the ASCII process table.
2. `watch` — Demonstrates real-time 1-second terminal refresh.
3. `pause heartbeat` -> `status` -> `resume heartbeat` — Demonstrates POSIX process signals.
4. `quit` — Demonstrates graceful shutdown sequence.

---

## 6. Viva & Oral Exam Q&A Defense

#### Q1: Why did you use `waitpid(..., WNOHANG)` instead of `wait()`?
> **Answer:** `wait()` is a blocking system call; it suspends the calling thread until any child terminates. In a supervisor that must simultaneously handle user CLI input, read pipe heartbeats, and serve HTTP web clients, blocking would freeze the entire system. `waitpid(-1, &status, WNOHANG)` returns immediately if no child has changed state, allowing our `select()` event loop to stay responsive.

#### Q2: How does the anonymous pipe heartbeat mechanism work, and why is it needed?
> **Answer:** While `waitpid()` tells us when a child *exits*, it cannot detect when a child *hangs, deadlocks, or spins in an infinite loop*. By allocating an anonymous pipe (`pipe()`) and passing write descriptor 3 (`SUPERVISOR_HB_FD`) to the child, the child periodically writes a single byte (`'H'`) every 2 seconds. The supervisor monitors the read end non-blockingly with `select()`. If no heartbeat arrives before `heartbeat_timeout_sec`, the watchdog marks the process `UNHEALTHY` and recovers it.

#### Q3: Why did you use unbuffered `open()` and `write()` instead of `fprintf()` for the audit log?
> **Answer:** Standard library I/O functions (`fprintf`, `fputs`) use userspace memory buffers (typically 4KB–8KB) to reduce system call overhead. If the supervisor process crashes unexpectedly or the machine suffers sudden power failure, buffered log data is lost. By using low-level POSIX `open()` with `O_APPEND` and direct `write()`, data is committed immediately and atomic appends are guaranteed.

#### Q4: How does the C supervisor serve the web console without using Node.js or NGINX at runtime?
> **Answer:** We built an embedded HTTP 1.1 server (`src/http_server.c`) in C. It creates a TCP socket, binds to port 8080, sets `O_NONBLOCK`, and registers the socket descriptor with the supervisor's central `select()` loop. When a browser connects, `select()` wakes up and we parse the HTTP request headers and stream static files (`index.html`, `styles.css`, `app.js`) or JSON payloads (`/api/status`, `/api/action`) directly using socket `write()`.

#### Q5: What is the purpose of `setsid()` in process spawning?
> **Answer:** By default, child processes created with `fork()` inherit their parent's session and process group. If a child spawns background worker threads or grandchildren, killing the child PID with `kill(pid, SIGTERM)` might leave orphan grandchild processes. By calling `setsid()`, the child becomes the session leader of a new process group (`PGID == PID`). When we stop the service, we call `killpg(svc->pgid, SIGTERM)`, which cleanly terminates the entire process subtree.

#### Q6: How does the supervisor prevent zombie processes?
> **Answer:** A zombie process occurs when a child terminates but its parent never reads its exit status from the kernel process table. Our supervisor installs a `SIGCHLD` handler with `sigaction()`. Whenever any child dies, the kernel raises `SIGCHLD`, and our event loop immediately calls `reap_children()` using `waitpid(-1, &status, WNOHANG)` in a loop until all dead children are collected.

#### Q7: What is exponential backoff and why is it used?
> **Answer:** If a service has a fatal configuration bug (e.g. missing database credentials or invalid port), restarting it immediately in a tight loop creates a "restart storm" that consumes 100% CPU and floods disk logs. Exponential backoff multiplies the restart delay by powers of 2 (`delay * 2^(attempts-1)`), giving external dependencies time to recover while protecting system resources.

#### Q8: Why did some services previously show "0 restarts and 2 crashes" while flaky showed many, and how did you resolve it?
> **Answer:** In traditional supervisor architectures, the consecutive restart counter resets to 0 once a service runs stably past its `start_secs` (2 seconds) threshold so that an intermittent failure hours later doesn't immediately abort due to hitting `max_restarts`. However, this previously erased the cumulative lifetime restart history from view. We solved this by decoupling the active backoff counter (`restart_count`) from the permanent lifetime recovery counter (`total_restarts`) in `service_t`. In addition, we updated `dummy_worker`, `flaky_worker`, and `hung_worker` to include randomized lifespans (~20s–44s) so that the supervisor's automatic recovery and restart telemetry can be observed dynamically across all services equally.

#### Q9: How do you demonstrate and test chaos engineering and concurrent multi-service execution?
> **Answer:** We built a dedicated `random_restart` endpoint (`POST /api/action {"action":"random_restart"}`) and wired it to `🎲 Random Restart` and `⚡ Auto-Chaos: ACTIVE (12s)` buttons in the Web Console. When invoked, the supervisor randomly selects a running service and initiates a controlled restart while immediately checking dependencies and ensuring 5 to 6 services continue running concurrently (`ensure_multiple_services_running`). To verify this automatically, we created an end-to-end Python test script (`test_all.py`) that executes 9 comprehensive test suites across signals, endpoints, logs, concurrency, and Linux `/proc` kernel process verification (external `SIGKILL`, `SIGHUP`, and 0 zombies).
