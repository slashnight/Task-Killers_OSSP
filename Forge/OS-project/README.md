# Linux Service and Process Supervisor

## Project title: Linux Service and Process Supervisor

**Team name**: task killers  
**Course**: Operating Systems  
**Language**: C11 (POSIX / Linux) & Modern Web UI (Vanilla JS / CSS)  
**Repository**: https://github.com/slashnight/KLH_CSE_2026-27_Team-22_Service-and-process-supervisor  

---

## Problem statement

A production Linux server runs many background services such as web servers, database services, monitoring agents, and scheduled tasks. If a service crashes or hangs, administrators need the system to detect the failure instantly, trigger controlled restarts, and maintain full observability over system telemetry.

The organization needs a lightweight service-management system that can start, stop, monitor, restart, and control background processes while maintaining their execution status, resource usage, and audit history.

This project was built as a full-featured Linux service supervisor that reads service definitions, creates processes using `fork()` and `execvp()`, isolates process groups with `setsid()`, monitors children non-blockingly with `waitpid(..., WNOHANG)`, tracks liveness using anonymous pipe heartbeats (`pipe()`), handles POSIX signals with `sigaction()`, provides an embedded non-blocking HTTP REST server (`src/http_server.c`), and exposes a real-time web dashboard with animated heartbeat telemetry.

---

## Team members and responsibilities

### Sai Kiran (2520030125) — Service Management & Initialization Engine
- Configuration parser in `src/config.c` (`load_config()`, dynamic reload with `SIGHUP`).
- Process creation and execution in `src/process.c` using `fork()` and `execvp()`.
- Working directory switching with `chdir()`, process grouping with `setsid()`.
- PID and PPID tracking using `getpid()` and `getppid()`.
- CLI commands and argument vector construction.

### Harshith (2520030393) — Process Monitoring, Signal Handling & Fault Recovery
- Non-blocking status polling and child reaping using `waitpid(-1, &status, WNOHANG)`.
- Signal interception using `sigaction()` with `SA_RESTART` and `SA_NOCLDSTOP`.
- Signal-based process control using `killpg()` (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`).
- Crash detection via `WIFEXITED()` / `WIFSIGNALED()` and automatic restart backoff.
- Multi-step graceful shutdown coordination.

### Nishanth (2520030323) — Resource Management, Audit Logging & IPC Interface
- Direct low-level logging using unbuffered POSIX system calls: `open()`, `write()`, `close()`.
- Resource accounting via `getrusage(RUSAGE_SELF, ...)` and `getrusage(RUSAGE_CHILDREN, ...)`.
- Heartbeat IPC communication through anonymous pipes (`pipe()`) over inherited file descriptor 3 (`SUPERVISOR_HB_FD`).
- Non-blocking heartbeat event multiplexing and watchdog timeout enforcement in the `select()` event loop.

---

## Functional requirements implemented

1. **Service configuration and process spawning**
   - The `service_t` structure stores the name, executable command, arguments, PID, PGID, restart attempts, state, and health intervals.
   - Services are defined declaratively in `config/services.conf`.
   - The supervisor creates isolated child execution spaces with `fork()`, sets independent process groups with `setsid()`, and executes the target binary with `execvp()`.

2. **Continuous monitoring and automatic recovery**
   - The event loop checks every child process non-blockingly using `waitpid(-1, &status, WNOHANG)`.
   - Both non-zero exit codes (`WEXITSTATUS`) and fatal signal terminations (`WTERMSIG`) are detected.
   - Failed services are automatically restarted with exponential backoff delay until `max_restarts` is reached.
   - Every crash and restart cycle is logged to `logs/supervisor.log`.

3. **Low-level unbuffered audit logging engine**
   - Implemented with direct POSIX file operations rather than buffered `FILE*` APIs.
   - Files are opened with `open(..., O_WRONLY | O_CREAT | O_APPEND, 0644)` and written with atomic `write()` calls.
   - Strict timestamp formatting: `[YYYY-MM-DD HH:MM:SS] [LEVEL] [PID: xxx] message`.

4. **Resource analytics and kernel metrics**
   - Child process CPU time and memory consumption are collected with `getrusage(RUSAGE_CHILDREN, &usage)`.
   - Supervisor memory is tracked with `getrusage(RUSAGE_SELF, ...)` and `/proc/[pid]/statm`.
   - Telemetry is exposed via terminal status tables and the `/api/status` endpoint.

5. **Inter-process communication (IPC) heartbeats and watchdog**
   - Anonymous pipes (`pipe()`) create unidirectional heartbeat communication channels.
   - Workers inherit write descriptor 3 (`SUPERVISOR_HB_FD`) and transmit `'H'` pulse characters every 2 seconds.
   - The supervisor drains pulse bytes non-blockingly inside `select()` via `drain_heartbeats()`, updating `svc->last_heartbeat`.
   - If a worker hangs or fails to beat within `heartbeat_timeout_sec`, the supervisor watchdog marks it `UNHEALTHY`, sends `SIGTERM` (escalating to `SIGKILL`), and triggers automatic recovery.

6. **POSIX signal handling and graceful shutdown**
   - Intercepts `SIGINT`, `SIGTERM`, `SIGCHLD`, and `SIGHUP` safely using `sigaction()`.
   - Controlled process signals: `SIGTERM` (graceful stop), `SIGKILL` (force kill), `SIGSTOP` (pause process), and `SIGCONT` (resume process).
   - On shutdown, `SIGTERM` is broadcast to all active child process groups. After a grace period, `SIGKILL` is sent to any stubborn processes before releasing all descriptors and locks.

7. **Embedded HTTP REST API Server (`src/http_server.c`)**
   - Non-blocking HTTP 1.1 server running directly on port `8080`, multiplexed inside the supervisor's `select()` event loop.
   - Serves static assets (`index.html`, `styles.css`, `app.js`) with cache-invalidation headers (`Cache-Control: no-cache, no-store, must-revalidate`).
   - REST API endpoints:
     - `GET /api/status`: JSON dump of supervisor PID, uptime, load averages, memory, and full service states with live heartbeat telemetry.
     - `POST /api/action`: Execute actions (`start`, `stop`, `restart`, `pause`, `resume`, `kill`, `start_all`, `stop_all`, `approve_waiting`, `reload`, `shutdown`). `stop_all` cleanly terminates all child worker processes via `SIGTERM` while keeping the HTTP supervisor server alive.
     - `GET /api/logs?service=<name>&lines=<n>`: Real-time log stream of supervisor audit logs or individual service output files (`logs/<name>.out`).
     - `GET /api/config` & `POST /api/config`: Inspect and edit configuration live with immediate `SIGHUP` hot reload.

8. **Web Console UI (`linux-supervisor-website/`)**
   - Dark-theme, high-performance web dashboard.
   - **Service Controls**: Dedicated `▶ Start All` and `⏹ Stop All` action buttons on both Dashboard and Services views.
   - **Heartbeat Pulse Column**: Real-time pulsing badge (`@keyframes hbPulseAnim`) displaying `Pulse (1s ago) / 6s max`.
   - **IPC Heartbeat Telemetry Monitor**: Status cards for each service displaying pipe(3) IPC state, watchdog threshold, and direct links to live service output logs.
   - **Worker Log Tagging**: Formats and highlights `[heartbeat]`, `[metrics]`, `[logger]`, and `[audit]` logs.
   - **Auto-Recovery Policy**: Ensures multiple services run concurrently whenever any service crashes or restarts.
   - **Robust DOM Lifecycle**: Initializes event handlers reliably across all document loading states (`DOMContentLoaded`, `interactive`, `complete`).

9. **Local developer launcher & synchronization (`dev.js`)**
   - Launchable via `npm run dev` or `npm start`.
   - Checks if the C supervisor backend is already active; if not, automatically launches WSL Ubuntu `./run.sh`.
   - **Continuous Health Monitor & Auto-Recovery**: Monitors backend status every 4 seconds. If the supervisor process terminates or goes offline, `dev.js` automatically relaunches it in WSL Ubuntu to maintain persistent service availability.
   - Opens the dashboard in the default browser using `start "" "http://localhost:8080/"`.
   - Enforces single-instance integrity using `logs/supervisor.lock` with POSIX `fcntl(F_SETLK)`.

---

## Project structure

```text
OS-project/
  Makefile                     # POSIX compilation rules with strict C11 flags
  package.json                 # Node.js development scripts (npm start, npm run dev)
  dev.js                       # Dev launcher & browser synchronizer
  run.sh                       # Ubuntu/WSL build, dependency check & execution script
  run.bat                      # Windows launcher delegating to WSL Ubuntu
  README.md                    # Project overview & documentation
  work.md                      # Team responsibilities & file mapping
  workflow.md                  # System architecture, event loops & system call traces
  understand.md                # In-depth OS concepts, APIs & lifecycle guide
  practice.md                  # Presentation, viva defense & Q&A guide
  config/
    services.conf              # Declarative service definitions & dependency tree
  include/
    supervisor.h               # Core types, service states, prototypes & macros
  src/
    supervisor.c               # Main CLI, select() event loop, watchdog timers, lock
    config.c                   # services.conf parser, validation & argv generation
    process.c                  # fork(), execvp(), waitpid(), pipe() & signal delivery
    signals.c                  # sigaction() handlers for SIGCHLD, SIGINT, SIGTERM, SIGHUP
    logger.c                   # Direct open(), write(), close() audit log & getrusage()
    http_server.c              # Embedded non-blocking HTTP REST server & static router
    dummy_worker.c             # Stable worker emitting periodic pipe(3) heartbeats
    flaky_worker.c             # Demo worker that fails to test restart backoff
    hung_worker.c              # Demo worker that stops heartbeats to test watchdog
  linux-supervisor-website/    # Modern responsive Web Console frontend
    index.html                 # UI structure (Dashboard, Services, Logs, Config)
    styles.css                 # Dark-theme CSS with heartbeat pulse animations
    app.js                     # REST polling, signal triggers, live log tailing
    README.md                  # Web console architecture & integration guide
  bin/                         # Compiled C binaries (supervisor, workers)
  logs/                        # Runtime outputs (supervisor.log, <service>.out)
```

---

## Core modules and file responsibilities

### `include/supervisor.h`
Contains:
- `service_t` and `supervisor_t` core data structures.
- Process state enumerations: `STOPPED`, `STARTING`, `RUNNING`, `PAUSED`, `FAILED`, `UNHEALTHY`, `WAITING`, `STOPPING`.
- Signal action definitions and IPC pipe descriptor macro (`SUPERVISOR_HB_FD = 3`).
- Function prototypes shared across all C modules.

### `src/config.c`
Responsible for:
- Parsing `config/services.conf` sections and `key=value` properties.
- Validating commands, working directories, timeouts, and dependencies (`depends_on`).
- Building tokenized `argv` arrays for process execution.
- Dynamic configuration reloading (`handle_reload()`) triggered via `SIGHUP` or `/api/config`.

### `src/process.c`
Responsible for:
- Creating child execution spaces with `fork()` and process images with `execvp()`.
- Process group isolation via `setsid()` / `setpgid()` for clean subtree signal delivery.
- Heartbeat anonymous pipe setup (`pipe()`), retaining write descriptor on fd 3.
- Non-blocking child reaping with `waitpid(-1, &status, WNOHANG)`.
- Process signal delivery (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`) via `killpg()`.

### `src/signals.c`
Responsible for:
- Setting up signal dispositions using POSIX `sigaction()` with `SA_RESTART` and `SA_NOCLDSTOP`.
- Safe signal interception for `SIGCHLD`, `SIGINT`, `SIGTERM`, and `SIGHUP`.
- Setting atomic signal flags consumed by the central `select()` event loop.

### `src/logger.c`
Responsible for:
- Unbuffered audit trail logging using low-level `open()`, `write()`, and `close()`.
- Formatted log headers: `[YYYY-MM-DD HH:MM:SS] [LEVEL] [PID: xxx] message`.
- Child resource accounting via `getrusage(RUSAGE_CHILDREN, ...)`.

### `src/http_server.c`
Responsible for:
- Embedded non-blocking HTTP REST server listening on `0.0.0.0:8080`.
- Serving static frontend files (`index.html`, `styles.css`, `app.js`) with cache control headers.
- Providing JSON endpoints: `/api/status`, `/api/action`, `/api/logs`, and `/api/config`.
- Executing web-initiated actions safely inside the supervisor's execution context.

### `src/supervisor.c`
The central supervisor entry point and event multiplexer:
- Manages the single-instance file lock (`logs/supervisor.lock`) via `fcntl(F_SETLK)`.
- Runs the `select()` event loop combining stdin CLI, HTTP sockets, and heartbeat pipes.
- Evaluates health watchdog timeouts and schedules exponential backoff restarts.
- Coordinates graceful multi-step shutdown (`SIGTERM` -> grace period -> `SIGKILL`).

---

## Execution flow

1. **Initialization**: `supervisor.c` acquires the instance lock (`logs/supervisor.lock`), opens the audit log with `open()`, and initializes signal handlers with `sigaction()`.
2. **Configuration Loading**: `config.c` parses `config/services.conf`, builds argument vectors, and registers all services into the in-memory `supervisor_t` table.
3. **HTTP Server Initialization**: `http_server.c` binds an IPv4 socket to port `8080`, sets non-blocking flags, and registers the listen socket with `select()`.
4. **Service Startup**: `process.c` evaluates dependency conditions (`depends_on`). Ready services create an anonymous pipe and call `fork()`.
5. **Child Execution**: The child calls `setsid()`, redirects standard output to `logs/<name>.out`, binds the pipe write end to fd 3, and invokes `execvp()`.
6. **Parent Event Multiplexing**: The parent adds the child's pipe read descriptor to the `select()` read set.
7. **Heartbeat Monitoring**: Workers transmit `'H'` pulses every 2 seconds. The supervisor reads pulses non-blockingly, updating `svc->last_heartbeat`.
8. **Watchdog Evaluation**: If `(now - last_heartbeat) >= heartbeat_timeout_sec`, the supervisor marks the service `UNHEALTHY`, sends `SIGTERM`, and initiates recovery.
9. **Child Reaping**: When a child exits, the kernel delivers `SIGCHLD`. The supervisor reaps the PID with `waitpid(-1, &status, WNOHANG)`, logs `getrusage()` metrics, and schedules auto-restart if enabled.
10. **Graceful Shutdown**: On `quit` or `SIGINT`/`SIGTERM`, the supervisor sends `SIGTERM` to all child process groups, waits for clean termination, sends `SIGKILL` if necessary, closes all file descriptors, removes the lock, and exits safely.

---

## How to build and run

### Method 1: Instant Dev Launcher (Recommended on Windows / Mac / Linux)

Run from the project root:

```bash
npm run dev
# or
npm start
```

This launches `dev.js`, which checks if the C supervisor backend is already active on port `8080`. If not, it automatically boots Ubuntu WSL, compiles the C binaries, starts the supervisor daemon, monitors backend health with auto-recovery, and launches the web console in your default browser at **http://localhost:8080/**.

### Method 2: Running Automated End-to-End Tests

To execute the comprehensive 9-suite automated test suite (which automatically starts the supervisor daemon if it is not already running):

```bash
npm test
# or
npm run test_all.py
# or inside WSL Ubuntu:
python3 test_all.py
```

> **Note:** The test command is `npm test` (or `npm run test_all.py`), which calls Python 3 inside Ubuntu WSL. (Running `npm test_all.py` directly without `run` is not recognized by npm).

To run the quick CLI smoke test:
```bash
npm run test:cli
```

### Method 3: Windows Batch Launcher

Double-click or run from PowerShell / Command Prompt:

```bat
run.bat
```

This delegates execution to Ubuntu WSL, runs dependency verification, compiles the project, and starts the supervisor.

### Method 4: Linux / Ubuntu WSL Script

```bash
chmod +x run.sh
./run.sh
```

`run.sh` automatically checks for `gcc` and `make`, installs them if missing via `apt`, compiles all binaries, and launches the supervisor.

### Method 5: Manual Compilation

```bash
make clean
make all
./bin/supervisor -c config/services.conf
```

Or via npm:
```bash
npm run build
npm run clean
```

---

## Terminal CLI commands reference

| Command | Purpose |
|---|---|
| `status` | Displays the live terminal table of all services, PIDs, states, uptimes, and restart counts |
| `watch` | Displays a continuously refreshed status view (press `q` or `Enter` to exit) |
| `start all` | Spawns all configured services whose dependencies are satisfied |
| `start <name>` | Spawns a specific service by name |
| `stop all` | Sends `SIGTERM` to all active child service process groups |
| `stop <name>` | Sends `SIGTERM` to the service's process group |
| `restart <name>` | Sends `SIGTERM`, waits for termination, and restarts the service |
| `pause <name>` | Suspends process execution using `SIGSTOP` |
| `resume <name>` | Resumes a suspended process using `SIGCONT` |
| `kill <name>` | Forces immediate termination using `SIGKILL` |
| `info <name>` | Displays detailed service metadata, lifetime restarts, and `/proc/<pid>/status` |
| `log [name]` | Displays the last 20 lines of `logs/supervisor.log` or `logs/<name>.out` |
| `reload` | Hot-reloads `config/services.conf` commands, dependencies, and restart policies |
| `y` | Starts all waiting services whose dependencies have become stable |
| `help` | Prints the list of available commands |
| `quit` / `shutdown` | Gracefully terminates all child processes and shuts down the supervisor |

---

## HTTP REST API reference

The embedded HTTP server listens on port **8080**:

| Endpoint | Method | Description |
|---|---|---|
| `GET /` | `GET` | Serves the web dashboard (`linux-supervisor-website/index.html`) |
| `GET /api/status` | `GET` | Returns full JSON telemetry: supervisor PID, uptime, load average, memory usage, and array of all services with PIDs, states, restarts (`restart_count` & `total_restarts`), crashes, and live heartbeat timestamps |
| `POST /api/action` | `POST` | Executes actions: `{"action":"start|stop|restart|pause|resume|kill", "service":"<name>"}` or batch actions `{"action":"start_all|stop_all|approve_waiting|reload|shutdown|random_restart"}`. `stop_all` stops all child services while keeping the HTTP supervisor server online. |
| `GET /api/logs` | `GET` | Streams log lines: `/api/logs?service=<name>&lines=120` or `/api/logs?source=supervisor` |
| `GET /api/config` | `GET` | Returns the raw contents of `config/services.conf` |
| `POST /api/config` | `POST` | Updates `config/services.conf` and immediately triggers dynamic `SIGHUP` hot reload |

---

## Configuration file format (`config/services.conf`)

```ini
[heartbeat]
command=./bin/dummy_worker heartbeat 2
autorestart=yes
max_restarts=10
delay=1
backoff=yes
heartbeat=yes
heartbeat_timeout=6
stop_timeout=3
start_secs=2
nice=5

[logger]
command=./bin/dummy_worker logger 3
depends_on=heartbeat
autorestart=yes
max_restarts=10
heartbeat=yes
heartbeat_timeout=8

[flaky]
command=./bin/flaky_worker
autorestart=yes
max_restarts=8
delay=1
backoff=yes
heartbeat=no
```

Configuration directives:
- `command`: Path to the executable binary and its arguments.
- `depends_on`: Name of the prerequisite service that must reach `RUNNING` state before this service starts.
- `autorestart`: Whether the supervisor should automatically restart the process on abnormal exit.
- `max_restarts`: Maximum number of consecutive restart attempts before marking as `FAILED`.
- `delay`: Base delay in seconds before triggering restart.
- `backoff`: Enables exponential backoff (`delay * 2^(attempts-1)`).
- `heartbeat`: Enables IPC anonymous pipe heartbeat tracking.
- `heartbeat_timeout`: Watchdog timeout in seconds before an unresponsive service is declared `UNHEALTHY`.
- `stop_timeout`: Grace period in seconds to wait for `SIGTERM` before escalating to `SIGKILL`.
- `nice`: Process scheduling priority adjustment via `nice()`.

---

## Tested behavior & verification

The project has been tested and verified across multiple full verification cycles:

1. **Compilation**: Strict `-Wall -Wextra -O2 -std=c11 -pedantic` compilation passes with zero warnings.
2. **Concurrent Multi-Service Execution**: All 6 configured services (`heartbeat`, `logger`, `metrics`, `audit`, `flaky`, `hung`) start and run concurrently.
3. **Equal Distributed Fault Recovery**: All workers seed PRNG uniquely per instance and run for randomized lifespans (~20s–44s) before exiting with code 1. Restarts and crashes are distributed equally across all services rather than only `flaky`.
4. **Cumulative Lifetime Accounting**: Total restarts are tracked in `svc->total_restarts` and never wiped out upon reaching stability, ensuring accurate long-term telemetry.
5. **Multi-Service Concurrency Enforcement**: The supervisor immediately starts dependent and unblocked services whenever any service crashes, maintaining 5 to 6 concurrently running services at all times.
6. **Chaos Engineering**: Dedicated `🎲 Random Restart` and `⚡ Auto-Chaos: ACTIVE (12s)` buttons allow live failure injection and automated recovery demonstration.
7. **End-to-End Automated Test Suite (`test_all.py`)**:
   ```bash
   python3 test_all.py
   ```
   Validates 9 comprehensive test suites:
   - `GET /api/status` (HTTP 200, valid JSON, PID)
   - Concurrency (all 6 services running simultaneously)
   - Chaos restart (`POST /api/action {"action":"random_restart"}`)
   - Cumulative restart counter increment verification
   - Signal dispatch (`SIGSTOP` pause & `SIGCONT` resume)
   - Stop and start cycles (`SIGTERM` & fork/exec)
   - Configuration API (`GET /api/config`)
   - Audit logging API (`GET /api/logs`)
   - High concurrency preservation over time across natural worker and watchdog recovery cycles
   - Real Linux OS kernel verification (`/proc/<pid>/status` `PPid` & `PGID == PID` isolation, `logs/<name>.pid` pidfiles, external `os.kill(pid, SIGKILL)` auto-reap & respawn, direct `SIGHUP` hot reload, and 0 zombie processes).

---

## Documentation reference

- [work.md](work.md) — Detailed team responsibilities, code mapping, and recovery architecture.
- [workflow.md](workflow.md) — Comprehensive architecture diagrams, state machines, and system call execution traces.
- [understand.md](understand.md) — Deep-dive educational explanation of all Operating System concepts, POSIX APIs, and edge cases.
- [practice.md](practice.md) — Presentation script, viva defense questions, and step-by-step evaluation walkthrough.
- [test_all.py](test_all.py) — Comprehensive automated Python test suite.
- [linux-supervisor-website/README.md](linux-supervisor-website/README.md) — Web console architecture, frontend-backend REST communication, and design system.
