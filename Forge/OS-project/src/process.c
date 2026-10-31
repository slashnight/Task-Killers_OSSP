#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#ifndef _WIN32
#include <sys/select.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "../include/supervisor.h"

static void format_uptime(time_t start, char *buf, size_t n) {
    if (start == 0) {
        snprintf(buf, n, "-");
        return;
    }
    long sec = (long)(time(NULL) - start);
    if (sec < 0) {
        sec = 0;
    }
    snprintf(buf, n, "%02ld:%02ld:%02ld", sec / 3600, (sec % 3600) / 60, sec % 60);
}

static void format_timestamp(time_t value, char *buf, size_t n) {
    if (value == 0) {
        snprintf(buf, n, "never");
        return;
    }
    struct tm tm_value;
    localtime_r(&value, &tm_value);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_value);
}

static int backoff_delay(const service_t *svc) {
    int delay = svc->restart_delay_sec > 0 ? svc->restart_delay_sec : 1;
    if (!svc->backoff) {
        return delay;
    }
    for (int i = 1; i < svc->restart_count && delay < MAX_BACKOFF_SEC; i++) {
        delay *= 2;
    }
    if (delay > MAX_BACKOFF_SEC) {
        delay = MAX_BACKOFF_SEC;
    }
    return delay;
}

static int dependencies_ready(const service_t *svc) {
    if (svc->depends_on[0] == '\0') {
        return 1;
    }
    service_t *dep = find_service_by_name(svc->depends_on);
    if (!dep) {
        return 0;
    }
    return dep->state == STATE_RUNNING && dep->stable;
}

void close_service_fds(service_t *svc) {
    if (svc->hb_fd >= 0) {
        close(svc->hb_fd);
        svc->hb_fd = -1;
    }
}

static void write_pidfile(const service_t *svc) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "logs/%s.pid", svc->name);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return;
    }
    fprintf(fp, "%d\n", (int)svc->pid);
    fclose(fp);
}

static void remove_pidfile(const service_t *svc) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "logs/%s.pid", svc->name);
    unlink(path);
}

static void close_extra_fds(int keep_fd) {
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 1024) {
        maxfd = 256;
    }
    for (int fd = 3; fd < maxfd; fd++) {
        if (fd != keep_fd) {
            close(fd);
        }
    }
}

static int send_to_group(service_t *svc, int sig) {
    pid_t target = svc->pgid > 0 ? svc->pgid : svc->pid;
    if (target <= 0) {
        errno = ESRCH;
        return -1;
    }
    if (killpg(target, sig) == 0) {
        return 0;
    }
    return kill(svc->pid, sig);
}

int start_service(service_t *svc) {
    if (!svc) {
        return -1;
    }
    if (svc->state == STATE_RUNNING || svc->state == STATE_PAUSED || svc->state == STATE_STOPPING) {
        log_event("WARN", "Service '%s' is already %s (pid %d)",
                  svc->name, state_to_string(svc->state), (int)svc->pid);
        return -1;
    }
    if (!dependencies_ready(svc)) {
        svc->state = STATE_WAITING;
        svc->waiting_notified = 0;
        log_event("INFO", "Service '%s' waiting on dependency '%s'",
                  svc->name, svc->depends_on);
        return 1;
    }

    int pipefd[2] = { -1, -1 };
    if (svc->heartbeat_enabled) {
        if (pipe(pipefd) < 0) {
            log_event("ERROR", "pipe() failed for '%s': %s", svc->name, strerror(errno));
            return -1;
        }
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_event("ERROR", "fork() failed for '%s': %s", svc->name, strerror(errno));
        svc->state = STATE_FAILED;
        if (pipefd[0] >= 0) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        if (setsid() < 0) {
            perror("setsid");
        }

        if (svc->workdir[0] && chdir(svc->workdir) < 0) {
            fprintf(stderr, "chdir(%s) failed: %s\n", svc->workdir, strerror(errno));
            _exit(126);
        }

        if (svc->nice_value != 0) {
            if (setpriority(PRIO_PROCESS, 0, svc->nice_value) < 0) {
                perror("setpriority");
            }
        }

        char outpath[MAX_PATH];
        snprintf(outpath, sizeof(outpath), "logs/%s.out", svc->name);
        int outfd = open(outpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (outfd >= 0) {
            dup2(outfd, STDOUT_FILENO);
            dup2(outfd, STDERR_FILENO);
            if (outfd > STDERR_FILENO) {
                close(outfd);
            }
        }

        if (svc->heartbeat_enabled) {
            close(pipefd[0]);
            if (pipefd[1] != HEARTBEAT_CHILD_FD) {
                dup2(pipefd[1], HEARTBEAT_CHILD_FD);
                close(pipefd[1]);
            }
            char fdstr[8];
            snprintf(fdstr, sizeof(fdstr), "%d", HEARTBEAT_CHILD_FD);
            setenv("SUPERVISOR_HB_FD", fdstr, 1);
            close_extra_fds(HEARTBEAT_CHILD_FD);
        } else {
            close_extra_fds(-1);
        }

        execvp(svc->argv[0], svc->argv);
        fprintf(stderr, "execvp(%s) failed: %s\n", svc->argv[0], strerror(errno));
        _exit(127);
    }

    if (svc->heartbeat_enabled) {
        close(pipefd[1]);
        svc->hb_fd = pipefd[0];
        svc->last_heartbeat = time(NULL);
    } else {
        svc->hb_fd = -1;
    }

    svc->pid = pid;
    svc->pgid = pid;
    svc->state = STATE_RUNNING;
    svc->expected_stop = 0;
    svc->stable = 0;
    svc->start_time = time(NULL);
    svc->last_start_time = svc->start_time;
    svc->start_count++;
    svc->waiting_notified = 0;
    svc->next_restart_at = 0;
    svc->stop_deadline = 0;
    write_pidfile(svc);
    log_event("INFO", "fork+exec '%s' pid=%d pgid=%d cmd='%s'",
              svc->name, (int)pid, (int)svc->pgid, svc->command);
    return 0;
}

int stop_service(service_t *svc, int sig) {
    if (!svc) return -1;
    if (svc->pid <= 0 ||
        (svc->state != STATE_RUNNING && svc->state != STATE_PAUSED &&
         svc->state != STATE_UNHEALTHY && svc->state != STATE_STOPPING)) {
        svc->expected_stop = 1;
        svc->next_restart_at = 0;
        svc->state = STATE_STOPPED;
        log_event("INFO", "Service '%s' stopped / already inactive", svc->name);
        return 0;
    }

    if (svc->state == STATE_PAUSED) {
        send_to_group(svc, SIGCONT);
    }

    svc->expected_stop = 1;
    if (svc->state != STATE_STOPPING) {
        svc->last_stop_time = time(NULL);
        svc->stop_count++;
    }
    svc->state = STATE_STOPPING;
    svc->stop_deadline = time(NULL) + (svc->stop_timeout_sec > 0 ? svc->stop_timeout_sec : 3);

    if (send_to_group(svc, sig) < 0) {
        log_event("ERROR", "signal %d failed for '%s': %s", sig, svc->name, strerror(errno));
        return -1;
    }
    log_event("INFO", "Sent signal %d to process group of '%s' (pid %d)",
              sig, svc->name, (int)svc->pid);
    return 0;
}

int pause_service(service_t *svc) {
    if (!svc) return -1;
    if (svc->state == STATE_PAUSED) return 0;
    if (svc->state != STATE_RUNNING || svc->pid <= 0) {
        log_event("WARN", "Cannot pause '%s' (state %s, pid %d)",
                  svc->name, state_to_string(svc->state), (int)svc->pid);
        return -1;
    }
    if (send_to_group(svc, SIGSTOP) < 0) {
        log_event("ERROR", "SIGSTOP failed for '%s': %s", svc->name, strerror(errno));
        return -1;
    }
    svc->state = STATE_PAUSED;
    log_event("INFO", "Paused '%s' with SIGSTOP (pid %d)", svc->name, (int)svc->pid);
    return 0;
}

int resume_service(service_t *svc) {
    if (!svc) return -1;
    if (svc->state == STATE_RUNNING) return 0;
    if (svc->state != STATE_PAUSED || svc->pid <= 0) {
        log_event("WARN", "Cannot resume '%s' (state %s, pid %d)",
                  svc->name, state_to_string(svc->state), (int)svc->pid);
        return -1;
    }
    if (send_to_group(svc, SIGCONT) < 0) {
        log_event("ERROR", "SIGCONT failed for '%s': %s", svc->name, strerror(errno));
        return -1;
    }
    svc->state = STATE_RUNNING;
    svc->last_heartbeat = time(NULL);
    log_event("INFO", "Resumed '%s' with SIGCONT (pid %d)", svc->name, (int)svc->pid);
    return 0;
}

int kill_service(service_t *svc) {
    return stop_service(svc, SIGKILL);
}

int start_all(void) {
    int started = 0;
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < g_supervisor.count; i++) {
            service_t *svc = &g_supervisor.services[i];
            if (svc->state == STATE_RUNNING || svc->state == STATE_PAUSED ||
                svc->state == STATE_STOPPING || svc->state == STATE_UNHEALTHY) {
                continue;
            }
            int rc = start_service(svc);
            if (rc == 0) {
                started++;
                progress = 1;
            }
        }
    }
    return started;
}

static void ensure_multiple_services_running(const service_t *trigger_svc) {
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *other = &g_supervisor.services[i];
        if (other == trigger_svc) continue;
        if (other->state == STATE_WAITING && dependencies_ready(other)) {
            log_event("INFO", "Auto-starting dependent service '%s' (triggered by '%s')",
                      other->name, trigger_svc ? trigger_svc->name : "system");
            start_service(other);
        } else if (other->state == STATE_STOPPED && other->autorestart && !other->expected_stop) {
            log_event("INFO", "Auto-starting stopped service '%s' to maintain concurrent execution",
                      other->name);
            start_service(other);
        }
    }
}

static void schedule_restart(service_t *svc) {
    if (!svc->autorestart) {
        log_event("WARN", "Service '%s' failed and autorestart is disabled", svc->name);
        return;
    }
    svc->total_restarts++;
    if (svc->restart_count >= svc->max_restarts) {
        log_event("WARN", "Service '%s' reached max restarts (%d); cool-down recovery in 4s",
                  svc->name, svc->max_restarts);
        svc->restart_count = 0;
        svc->next_restart_at = time(NULL) + 4;
        return;
    }
    svc->restart_count++;
    int delay = backoff_delay(svc);
    svc->next_restart_at = time(NULL) + delay;
    log_event("INFO", "Scheduling restart (attempt %d, total %d) of '%s' in %d second(s)",
              svc->restart_count, svc->total_restarts, svc->name, delay);
}

static void handle_child_exit(service_t *svc, int status) {
    svc->last_exit_time = time(NULL);
    svc->last_exit_status = -1;
    svc->last_term_signal = 0;
    if (!svc->expected_stop) {
        svc->last_stop_time = svc->last_exit_time;
    }

    if (WIFEXITED(status)) {
        svc->last_exit_status = WEXITSTATUS(status);
        log_event("INFO", "waitpid: '%s' pid=%d exited status=%d",
                  svc->name, (int)svc->pid, svc->last_exit_status);
    } else if (WIFSIGNALED(status)) {
        svc->last_term_signal = WTERMSIG(status);
        log_event("INFO", "waitpid: '%s' pid=%d killed by signal %d (%s)",
                  svc->name, (int)svc->pid, svc->last_term_signal,
                  strsignal(svc->last_term_signal));
    }
    log_rusage(svc->name);

    int was_expected = svc->expected_stop;
    close_service_fds(svc);
    remove_pidfile(svc);
    svc->pid = -1;
    svc->pgid = -1;
    svc->start_time = 0;
    svc->stable = 0;
    svc->stop_deadline = 0;
    if (was_expected) {
        svc->state = STATE_STOPPED;
        svc->expected_stop = 1;
        log_event("INFO", "Service '%s' stopped as requested", svc->name);
        return;
    }

    svc->expected_stop = 0;

    svc->state = STATE_FAILED;
    svc->crash_count++;
    log_event("WARN", "Service '%s' exited/crashed; ensuring multiple services continue running", svc->name);
    ensure_multiple_services_running(svc);
    schedule_restart(svc);
}

void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        service_t *svc = find_service_by_pid(pid);
        if (!svc) {
            log_event("WARN", "Reaped unknown child pid %d", (int)pid);
            continue;
        }
        handle_child_exit(svc, status);
    }
    if (pid < 0 && errno != ECHILD) {
        log_event("ERROR", "waitpid() failed: %s", strerror(errno));
    }
}

void process_restarts(void) {
    time_t now = time(NULL);
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < g_supervisor.count; i++) {
            service_t *svc = &g_supervisor.services[i];
            if (svc->state == STATE_WAITING && dependencies_ready(svc)) {
                log_event("INFO", "Dependencies ready: auto-starting '%s'", svc->name);
                if (start_service(svc) == 0) {
                    progress = 1;
                }
            }
            if ((svc->state == STATE_FAILED || svc->state == STATE_STOPPED) &&
                svc->next_restart_at != 0 && now >= svc->next_restart_at) {
                svc->next_restart_at = 0;
                log_event("INFO", "Auto-restarting service '%s'", svc->name);
                if (start_service(svc) == 0) {
                    progress = 1;
                    ensure_multiple_services_running(svc);
                }
            }
            if (svc->state == STATE_RUNNING && !svc->stable && svc->start_time &&
                now - svc->start_time >= svc->start_secs) {
                svc->stable = 1;
                svc->restart_count = 0;
                log_event("INFO", "Service '%s' is stable after %d second(s)",
                          svc->name, svc->start_secs);
                ensure_multiple_services_running(svc);
            }
        }
    }
}

int start_waiting_services(void) {
    int started = 0;
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        if (svc->state == STATE_WAITING && dependencies_ready(svc) && start_service(svc) == 0) {
            started++;
        }
    }
    if (started == 0) {
        log_event("INFO", "No ready waiting services to start");
    } else {
        log_event("INFO", "Started %d approved waiting service(s)", started);
    }
    return started;
}

void refresh_service_states(void) {
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];

        if (svc->pid <= 0) {
            continue;
        }

        if (svc->state == STATE_PAUSED || svc->state == STATE_STOPPING ||
            svc->state == STATE_UNHEALTHY) {
            continue;
        }

        if (kill(svc->pid, 0) == 0 || errno == EPERM) {
            svc->state = STATE_RUNNING;
        }
    }
}

void check_health(void) {
    time_t now = time(NULL);
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        if (svc->pid <= 0 || svc->state != STATE_RUNNING) {
            continue;
        }
        if (kill(svc->pid, 0) < 0 && errno == ESRCH) {
            log_event("WARN", "Liveness check failed for '%s' (ESRCH)", svc->name);
            continue;
        }
        if (!svc->heartbeat_enabled || svc->hb_fd < 0) {
            continue;
        }
        if (now - svc->last_heartbeat > svc->heartbeat_timeout_sec) {
            svc->state = STATE_UNHEALTHY;
            log_event("ERROR", "Heartbeat timeout for '%s' (%ds) — restarting with SIGKILL",
                      svc->name, svc->heartbeat_timeout_sec);
            svc->expected_stop = 0;
            send_to_group(svc, SIGKILL);
        }
    }
}

void enforce_stop_timeouts(void) {
    time_t now = time(NULL);
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        if ((svc->state == STATE_STOPPING || svc->state == STATE_UNHEALTHY) && svc->pid > 0 &&
            svc->stop_deadline != 0 && now >= svc->stop_deadline) {
            log_event("WARN", "Stop timeout for '%s' — sending SIGKILL", svc->name);
            send_to_group(svc, SIGKILL);
            svc->stop_deadline = 0;
        }
    }
}

void add_heartbeat_fds(fd_set *rfds, int *maxfd) {
    for (int i = 0; i < g_supervisor.count; i++) {
        int fd = g_supervisor.services[i].hb_fd;
        if (fd >= 0) {
            FD_SET(fd, rfds);
            if (fd > *maxfd) {
                *maxfd = fd;
            }
        }
    }
}

void drain_heartbeats(fd_set *rfds) {
    char buf[64];
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        if (svc->hb_fd < 0 || !FD_ISSET(svc->hb_fd, rfds)) {
            continue;
        }
        ssize_t n;
        int got = 0;
        while ((n = read(svc->hb_fd, buf, sizeof(buf))) > 0) {
            got = 1;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_event("WARN", "Heartbeat read error on '%s': %s", svc->name, strerror(errno));
        } else if (n == 0) {
            close_service_fds(svc);
        }
        if (got) {
            svc->last_heartbeat = time(NULL);
            if (svc->state == STATE_UNHEALTHY) {
                svc->state = STATE_RUNNING;
            }
        }
    }
}

void shutdown_all(void) {
    log_event("INFO", "Controlled shutdown of all managed services");
    for (int i = g_supervisor.count - 1; i >= 0; i--) {
        service_t *svc = &g_supervisor.services[i];
        if (svc->pid > 0) {
            stop_service(svc, SIGTERM);
        }
    }

    for (int attempt = 0; attempt < 40; attempt++) {
        reap_children();
        enforce_stop_timeouts();
        int alive = 0;
        for (int i = 0; i < g_supervisor.count; i++) {
            if (g_supervisor.services[i].pid > 0) {
                alive++;
            }
        }
        if (alive == 0) {
            break;
        }
        usleep(100000);
    }

    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        if (svc->pid > 0) {
            log_event("WARN", "Force killing '%s' pid %d", svc->name, (int)svc->pid);
            send_to_group(svc, SIGKILL);
        }
        close_service_fds(svc);
        remove_pidfile(svc);
    }
    reap_children();
    g_supervisor.running = 0;
    print_final_summary();
    log_event("INFO", "All services shut down");
}

static const char *state_color(service_state_t state) {
    switch (state) {
        case STATE_RUNNING:   return "\033[1;32m";
        case STATE_PAUSED:    return "\033[1;33m";
        case STATE_FAILED:    return "\033[1;31m";
        case STATE_UNHEALTHY: return "\033[1;35m";
        case STATE_STOPPING:  return "\033[1;36m";
        case STATE_WAITING:   return "\033[1;34m";
        default:              return "\033[0;37m";
    }
}

void print_status(void) {
    printf("\n");
    printf("  \033[1mLinux Service Supervisor\033[0m  team: task killers    pid %d\n",
           (int)getpid());
    printf("+------------------+--------+------------+----------+----------+----------+\n");
    printf("| %-16s | %-6s | %-10s | %-8s | %-8s | %-8s |\n",
           "SERVICE", "PID", "STATE", "RESTARTS", "CRASHES", "UPTIME");
    printf("+------------------+--------+------------+----------+----------+----------+\n");

    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *svc = &g_supervisor.services[i];
        char uptime[24];
        char pidbuf[16];
        format_uptime(svc->start_time, uptime, sizeof(uptime));
        if (svc->pid > 0) {
            snprintf(pidbuf, sizeof(pidbuf), "%d", (int)svc->pid);
        } else {
            snprintf(pidbuf, sizeof(pidbuf), "-");
        }
        printf("| %-16s | %-6s | %s%-10s\033[0m | %4d/%-3d | %8d | %-8s |\n",
               svc->name, pidbuf, state_color(svc->state), state_to_string(svc->state),
               svc->total_restarts, svc->max_restarts, svc->crash_count, uptime);
    }
    printf("+------------------+--------+------------+----------+----------+----------+\n");
    printf("  %d registered  |  heartbeat pipes + waitpid + signal control\n\n",
           g_supervisor.count);
}

    void print_final_summary(void) {
        printf("\nFinal service summary\n");
        printf("+------------------+--------+--------+----------+---------------------+\n");
        printf("| %-16s | %-6s | %-6s | %-8s | %-19s |\n",
            "SERVICE", "STARTS", "STOPS", "CRASHES", "LAST STOP");
        printf("+------------------+--------+--------+----------+---------------------+\n");
        for (int i = 0; i < g_supervisor.count; i++) {
         service_t *svc = &g_supervisor.services[i];
         char last_stop[32];
         format_timestamp(svc->last_stop_time, last_stop, sizeof(last_stop));
         printf("| %-16s | %6d | %6d | %8d | %-19s |\n",
             svc->name, svc->start_count, svc->stop_count,
             svc->crash_count, last_stop);
        }
        printf("+------------------+--------+--------+----------+---------------------+\n\n");
        for (int i = 0; i < g_supervisor.count; i++) {
            service_t *svc = &g_supervisor.services[i];
            char last_stop[32];
            format_timestamp(svc->last_stop_time, last_stop, sizeof(last_stop));
            log_event("INFO", "Final summary '%s': starts=%d stops=%d crashes=%d last_stop=%s",
                      svc->name, svc->start_count, svc->stop_count,
                      svc->crash_count, last_stop);
        }
    }

void print_service_info(const service_t *svc) {
    char uptime[24];
    format_uptime(svc->start_time, uptime, sizeof(uptime));
    printf("\nService: %s\n", svc->name);
    printf("  command     : %s\n", svc->command);
    printf("  workdir     : %s\n", svc->workdir[0] ? svc->workdir : "(supervisor cwd)");
    printf("  depends_on  : %s\n", svc->depends_on[0] ? svc->depends_on : "(none)");
    printf("  state       : %s%s\033[0m\n", state_color(svc->state), state_to_string(svc->state));
    printf("  pid / pgid  : %d / %d\n", (int)svc->pid, (int)svc->pgid);
    printf("  uptime      : %s\n", uptime);
    printf("  restarts    : %d total (%d active / %d max)\n", svc->total_restarts, svc->restart_count, svc->max_restarts);
    printf("  crashes     : %d\n", svc->crash_count);
    printf("  last status : %d\n", svc->last_exit_status);
    printf("  last signal : %d\n", svc->last_term_signal);
    printf("  heartbeat   : %s (timeout %ds, last %lds ago)\n",
           svc->heartbeat_enabled ? "on" : "off",
           svc->heartbeat_timeout_sec,
           svc->last_heartbeat ? (long)(time(NULL) - svc->last_heartbeat) : -1L);
    printf("  nice        : %d\n", svc->nice_value);

    if (svc->pid > 0) {
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/%d/status", (int)svc->pid);
        FILE *fp = fopen(procpath, "r");
        if (fp) {
            char line[MAX_LINE];
            printf("  /proc/%d/status:\n", (int)svc->pid);
            int shown = 0;
            while (fgets(line, sizeof(line), fp) && shown < 8) {
                if (strncmp(line, "Name:", 5) == 0 || strncmp(line, "State:", 6) == 0 ||
                    strncmp(line, "PPid:", 5) == 0 || strncmp(line, "VmRSS:", 6) == 0 ||
                    strncmp(line, "Threads:", 8) == 0 || strncmp(line, "NSpid:", 6) == 0) {
                    printf("    %s", line);
                    shown++;
                }
            }
            fclose(fp);
        }
    }
    printf("\n");
}

void print_recent_log(const char *name, int lines) {
    char path[MAX_PATH];
    if (name) {
        snprintf(path, sizeof(path), "logs/%s.out", name);
    } else {
        snprintf(path, sizeof(path), "%s", g_supervisor.log_path);
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("cannot open %s\n", path);
        return;
    }

    char **ring = calloc((size_t)lines, sizeof(char *));
    int count = 0;
    int idx = 0;
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), fp)) {
        free(ring[idx]);
        ring[idx] = strdup(buf);
        idx = (idx + 1) % lines;
        if (count < lines) {
            count++;
        }
    }
    fclose(fp);

    int start = (count == lines) ? idx : 0;
    printf("\n--- last %d line(s) of %s ---\n", count, path);
    for (int i = 0; i < count; i++) {
        int pos = (start + i) % lines;
        fputs(ring[pos], stdout);
        free(ring[pos]);
    }
    free(ring);
    printf("\n");
}

int acquire_instance_lock(const char *path) {
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("open lock");
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "Another supervisor is already running.\n");
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) == 0) {
        char pidbuf[32];
        int n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        if (write(fd, pidbuf, (size_t)n) < 0) {
            /* lock is still held; pid text is best-effort */
        }
    }
    g_supervisor.lock_fd = fd;
    return 0;
}

void release_instance_lock(void) {
    if (g_supervisor.lock_fd >= 0) {
        flock(g_supervisor.lock_fd, LOCK_UN);
        close(g_supervisor.lock_fd);
        g_supervisor.lock_fd = -1;
        unlink(DEFAULT_LOCK_PATH);
    }
}
