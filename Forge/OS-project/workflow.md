# Project Architecture & Operational Workflow

## 1. Project Information

- **Team:** task killers  
- **Project Title:** Linux Service and Process Supervisor  
- **Course:** Operating Systems  
- **Language:** C11 with POSIX/Linux APIs & Modern Web Console (HTML5/CSS3/JavaScript)  
- **Repository:** https://github.com/slashnight/KLH_CSE_2026-27_Team-22_Service-and-process-supervisor  

---

## 2. End-to-End System Architecture

The supervisor is an OS-level parent process that manages multiple isolated child worker processes while concurrently serving an embedded HTTP REST API and real-time Web Console.

```mermaid
flowchart TD
    subgraph Client Layer
        Web[Browser Web Console :8080]
        CLI[Terminal CLI / stdin]
        Dev[dev.js / npm run dev]
    end

    subgraph Supervisor Parent Process
        Main[src/supervisor.c : main]
        Lock[fcntl F_SETLK logs/supervisor.lock]
        Sig[src/signals.c : sigaction handlers]
        Conf[src/config.c : services.conf parser]
        HTTP[src/http_server.c : embedded non-blocking server]
        Proc[src/process.c : process engine]
        Log[src/logger.c : unbuffered open/write/close]
        EventLoop[select Event Multiplexing Loop]
    end

    subgraph Kernel & Subsystem
        Fork[fork]
        Exec[execvp]
        Wait[waitpid WNOHANG]
        Pipe[pipe IPC fd 3]
        Signals[SIGTERM / SIGKILL / SIGSTOP / SIGCONT]
        Usage[getrusage RUSAGE_CHILDREN]
    end

    subgraph Managed Child Processes
        W1[heartbeat : dummy_worker]
        W2[logger : dummy_worker]
        W3[metrics : dummy_worker]
        W4[audit : dummy_worker]
        W5[flaky : flaky_worker]
        W6[hung : hung_worker]
    end

    Dev --> Main
    Web <-->|HTTP GET/POST| HTTP
    CLI --> Main
    Main --> Lock
    Main --> Sig
    Main --> Conf
    Main --> HTTP
    Main --> EventLoop
    
    EventLoop -->|stdin| Proc
    EventLoop -->|HTTP Socket| HTTP
    EventLoop -->|Pipe read fd| EventLoop
    
    Proc --> Fork --> Exec
    Exec --> W1 & W2 & W3 & W4 & W5 & W6
    
    W1 & W2 & W3 & W4 & W6 -->|write 'H'| Pipe
    Pipe -->|drain_heartbeats| EventLoop
    
    Sig -->|SIGCHLD| Wait
    Wait --> Proc
    Proc --> Log
    Proc --> Usage
    Proc --> Signals --> W1 & W2 & W3 & W4 & W5 & W6
```

---

## 3. Core Module & File Map

| File | Primary Responsibility | Key Functions & System Calls |
|---|---|---|
| `include/supervisor.h` | Shared structures, state enums, IPC constants, prototypes | `service_t`, `supervisor_t`, `SUPERVISOR_HB_FD=3` |
| `src/supervisor.c` | Program entry, single-instance lock, `select()` multiplexing loop | `main()`, `acquire_instance_lock()`, `event_loop()` |
| `src/config.c` | INI configuration parsing, argument splitting, dynamic hot reload | `load_config()`, `parse_command()`, `handle_reload()` |
| `src/process.c` | Process spawning, signal delivery, non-blocking reaping, watchdog | `start_service()`, `stop_service()`, `reap_children()`, `check_health()` |
| `src/signals.c` | Signal interceptors for `SIGCHLD`, `SIGINT`, `SIGTERM`, `SIGHUP` | `sigaction()`, `install_signal_handlers()` |
| `src/logger.c` | Low-level file logging and resource accounting | `open()`, `write()`, `close()`, `getrusage()` |
| `src/http_server.c` | Embedded non-blocking HTTP REST server & static router | `socket()`, `bind()`, `listen()`, `select()`, `send_http_response()`, `stop_all` |
| `src/dummy_worker.c` | Stable worker emitting periodic heartbeat bytes over fd 3 | `write(3, "H", 1)` every 2 seconds |
| `src/flaky_worker.c` | Fault testing worker that crashes with exit code 1 | Tests crash detection and restart backoff |
| `src/hung_worker.c` | Watchdog testing worker that stalls heartbeats after 12–20s | Tests watchdog timeout detection (`UNHEALTHY` -> `SIGKILL`) and recovery |
| `linux-supervisor-website/` | Web Console frontend (Dashboard, Process Table, Logs, Config) | `index.html`, `styles.css`, `app.js` (`Start All`, `Stop All`) |
| `dev.js` | Cross-platform Node.js launcher & health auto-recovery | `checkServer()`, `openBrowser()`, auto-restart WSL backend |
| `config/services.conf` | Declarative service configuration and dependency specifications | 6 configured services with restart policies |
| `Makefile` | Build automation with strict C11 POSIX compiler flags | `make all`, `make clean`, `make test` |
| `run.sh` / `run.bat` | Automated Linux and Windows environment execution scripts | Dependency check, compilation, launcher |

---

## 4. Central `select()` Event Multiplexing Loop

The supervisor never blocks indefinitely on any single operation. Everything is multiplexed in a single POSIX `select()` event loop:

```mermaid
flowchart TD
    Loop[Start Event Loop Tick] --> SigCheck{Signal Flags Pending?}
    SigCheck -- SIGCHLD --> Reap[reap_children: waitpid WNOHANG]
    SigCheck -- SIGHUP --> Reload[handle_reload: hot reload config]
    SigCheck -- SIGINT/SIGTERM --> Shutdown[shutdown_all: graceful exit]
    
    Reap --> Timers[process_restarts & check_health]
    Reload --> Timers
    SigCheck -- None --> Timers
    
    Timers --> FDSet[Build select read fd_set: stdin + HTTP listen + HTTP clients + pipe fds]
    FDSet --> Select[select max_fd + 1, read_set, NULL, NULL, 250ms timeout]
    
    Select --> HandleEvents{Which descriptor is readable?}
    HandleEvents -- stdin --> HandleCLI[Read & execute terminal command]
    HandleEvents -- HTTP listen fd --> AcceptClient[accept client connection]
    HandleEvents -- HTTP client fd --> ParseHTTP[Parse HTTP request & send response]
    HandleEvents -- Pipe read fd --> DrainPulse[drain_heartbeats: update last_heartbeat]
    HandleEvents -- Timeout 250ms --> Loop
    
    HandleCLI --> Loop
    AcceptClient --> Loop
    ParseHTTP --> Loop
    DrainPulse --> Loop
```

---

## 5. Heartbeat IPC & Watchdog Recovery Workflow

```mermaid
sequenceDiagram
    autonumber
    participant Worker as Child Worker (dummy_worker)
    participant Pipe as Anonymous Pipe (fd 3)
    participant Supervisor as Supervisor (select loop)
    participant Watchdog as Health Watchdog (check_health)
    participant Kernel as Kernel Signals

    Note over Worker, Supervisor: Process Spawning (fork + execvp)
    Supervisor->>Pipe: pipe(pipefd) -> keep read end, pass write end as fd 3
    Supervisor->>Worker: fork() + execvp()
    
    rect rgb(235, 250, 240)
    Note over Worker, Supervisor: Normal Heartbeat Pulse Cycle (Every 2s)
    Worker->>Pipe: write(3, "H", 1)
    Pipe-->>Supervisor: select() wakes on readable pipefd
    Supervisor->>Supervisor: drain_heartbeats() -> update svc->last_heartbeat = now
    end

    rect rgb(255, 240, 240)
    Note over Worker, Watchdog: Fault Scenario (Worker Hangs / Stops Beating)
    Worker--xPipe: Worker hangs (stops transmitting 'H')
    Supervisor->>Watchdog: check_health() evaluates (now - last_heartbeat)
    Watchdog->>Supervisor: (now - last_heartbeat) >= heartbeat_timeout_sec
    Supervisor->>Supervisor: Mark state STATE_UNHEALTHY & log warning
    Supervisor->>Kernel: killpg(pgid, SIGTERM)
    alt Process ignores SIGTERM
        Supervisor->>Kernel: After stop_timeout_sec: killpg(pgid, SIGKILL)
    end
    Kernel-->>Supervisor: SIGCHLD
    Supervisor->>Supervisor: waitpid(-1, &status, WNOHANG) reaps child
    Supervisor->>Supervisor: schedule_restart() with backoff delay
    Supervisor->>Worker: Respawn child worker via fork() + execvp()
    end
```

---

## 6. HTTP REST API & Web Console Flow

```mermaid
sequenceDiagram
    participant UI as Web Console (app.js)
    participant Server as Embedded HTTP Server (http_server.c)
    participant Sup as Supervisor Core (supervisor.c)
    participant Worker as Worker Processes

    rect rgb(240, 245, 255)
    Note over UI, Server: Real-Time Status Polling (Every 1s)
    UI->>Server: GET /api/status (Cache-Control: no-cache)
    Server->>Sup: Read supervisor_t & service_t memory tables
    Sup-->>Server: Return PIDs, PGIDs, states, memory, last_heartbeat_ago
    Server-->>UI: HTTP 200 OK (JSON telemetry payload)
    UI->>UI: Render Process Table, pulsing green badges, stats
    end

    rect rgb(255, 245, 235)
    Note over UI, Worker: Web-Initiated Signal Action
    UI->>Server: POST /api/action {"action":"restart","service":"metrics"}
    Server->>Sup: stop_service(metrics, SIGTERM) -> schedule restart
    Sup->>Worker: killpg(pgid, SIGTERM)
    Server-->>UI: HTTP 200 OK {"success":true,"message":"Service restarted"}
    UI->>UI: Show success toast & trigger immediate pollStatus()
    end

    rect rgb(245, 255, 245)
    Note over UI, Server: Live Log Streaming
    UI->>Server: GET /api/logs?service=heartbeat&lines=120
    Server->>Server: Read last 120 lines from logs/heartbeat.out
    Server-->>UI: HTTP 200 OK {"service":"heartbeat","lines":[...]}
    UI->>UI: Format lines with [heartbeat] green badges in terminal view
    end
```

---

## 7. Service State Machine

```mermaid
stateDiagram-v2
    [*] --> STOPPED
    
    STOPPED --> WAITING : Dependencies not ready
    WAITING --> STARTING : Dependencies become stable
    STOPPED --> STARTING : start / start_all command
    
    STARTING --> RUNNING : fork() + execvp() succeeds
    STARTING --> FAILED : execvp() fails
    
    RUNNING --> PAUSED : SIGSTOP (pause action)
    PAUSED --> RUNNING : SIGCONT (resume action)
    
    RUNNING --> STOPPING : stop command (SIGTERM)
    STOPPING --> STOPPED : waitpid reaped cleanly
    STOPPING --> STOPPED : Escalated SIGKILL after timeout
    
    RUNNING --> UNHEALTHY : Heartbeat watchdog timeout
    UNHEALTHY --> FAILED : SIGTERM / SIGKILL recovery
    
    RUNNING --> FAILED : Crash / non-zero exit / signal
    FAILED --> STARTING : schedule_restart (backoff delay)
    FAILED --> STOPPED : Max restart attempts reached
```

---

## 8. Service Dependency Tree

```mermaid
flowchart TD
    HB[heartbeat : dummy_worker]
    LOG[logger : dummy_worker]
    MET[metrics : dummy_worker]
    AUD[audit : dummy_worker]
    FLK[flaky : flaky_worker]
    HNG[hung : hung_worker]

    HB -->|depends_on| LOG
    HB -->|depends_on| MET
    LOG -->|depends_on| AUD

    classDef stable fill:#e3f7ee,stroke:#168e62,stroke-width:2px;
    classDef demo fill:#fef3c7,stroke:#d97706,stroke-width:2px;
    
    class HB,LOG,MET,AUD stable;
    class FLK,HNG demo;
```

- `heartbeat` starts immediately at boot.
- `logger` and `metrics` wait in `STATE_WAITING` until `heartbeat` is running and stable.
- `audit` waits in `STATE_WAITING` until `logger` is running and stable.
- `flaky` and `hung` are independent demo workers for testing failure and watchdog recovery.

---

## 9. Comprehensive Verification Checklist

- [x] Strict C11 compilation (`gcc -Wall -Wextra -O2 -std=c11 -pedantic`) with zero warnings.
- [x] Multi-process isolation (`fork`, `execvp`, `setsid`, `killpg`).
- [x] Non-blocking process reaping (`waitpid(-1, &status, WNOHANG)`).
- [x] Anonymous pipe IPC heartbeats over inherited fd 3 (`SUPERVISOR_HB_FD`).
- [x] Non-blocking pulse draining inside `select()` via `drain_heartbeats()`.
- [x] Watchdog timeout enforcement and automated recovery for hung workers.
- [x] Crash detection (`WIFEXITED`, `WIFSIGNALED`) and exponential backoff retry.
- [x] Unbuffered POSIX audit logging (`open`, `write`, `close`).
- [x] Kernel resource tracking (`getrusage(RUSAGE_SELF)`, `getrusage(RUSAGE_CHILDREN)`).
- [x] Embedded HTTP REST server on port `8080` integrated into `select()` with `stop_all` and `shutdown` actions.
- [x] Cache prevention headers (`Cache-Control: no-cache, no-store, must-revalidate`).
- [x] Real-time Web Console with live animated heartbeat pulse badges and `Start All` / `Stop All` controls.
- [x] Cross-platform Node.js launcher (`npm run dev` / `dev.js`) with single-instance lock and automatic backend recovery.
- [x] Equal distributed fault recovery: all services cycle through lifespans (~20s–44s) and recover equally.
- [x] Cumulative lifetime restart counter (`total_restarts`) preserving total recovery events across stability transitions.
- [x] Multi-service concurrency preservation (`ensure_multiple_services_running`) maintaining 5–6 running services concurrently at all times.
- [x] Random chaos restart API endpoint (`POST /api/action` with `random_restart`).
- [x] Web Console Chaos Mode toolbar toggle (`⚡ Auto-Chaos: ACTIVE (12s)`).
- [x] Automated end-to-end Python test suite runnable via `npm test`, `npm run test_all.py`, or `python3 test_all.py` validating all 9 API, operational, and Linux `/proc` kernel verification suites (including external `SIGKILL` recovery, `SIGHUP` hot reload, and zero-zombie verification).
