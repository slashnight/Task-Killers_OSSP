#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "supervisor.h"

supervisor_t g_supervisor;

const char *state_to_string(service_state_t state) {
    switch (state) {
        case STATE_STOPPED:   return "STOPPED";
        case STATE_WAITING:   return "WAITING";
        case STATE_RUNNING:   return "RUNNING";
        case STATE_PAUSED:    return "PAUSED";
        case STATE_STOPPING:  return "STOPPING";
        case STATE_FAILED:    return "FAILED";
        case STATE_UNHEALTHY: return "UNHEALTHY";
        default:              return "UNKNOWN";
    }
}

service_t *find_service_by_name(const char *name) {
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < g_supervisor.count; i++) {
        if (strcmp(g_supervisor.services[i].name, name) == 0) {
            return &g_supervisor.services[i];
        }
    }
    return NULL;
}

service_t *find_service_by_pid(pid_t pid) {
    for (int i = 0; i < g_supervisor.count; i++) {
        if (g_supervisor.services[i].pid == pid) {
            return &g_supervisor.services[i];
        }
    }
    return NULL;
}

static void print_banner(void) {
    printf("\033[1;36m");
    printf("===============================================================\n");
    printf("  Linux Service and Process Supervisor  |  team: task killers\n");
    printf("  fork/exec  waitpid  signals  pipes  process groups  watchdog\n");
    printf("  Web Dashboard: http://localhost:%d\n", g_supervisor.http_port > 0 ? g_supervisor.http_port : 8080);
    printf("===============================================================\n");
    printf("\033[0m");
}

static void print_help(void) {
    printf(
        "\nCommands (terminal only):\n"
        "  status                 Show all registered services\n"
        "  y                      Start waiting services that are ready\n"
        "  watch                  Live-refresh status until Enter is pressed\n"
        "  start <name|all>       Start one service or every eligible service\n"
        "  stop <name|all>        SIGTERM the process group (then SIGKILL on timeout)\n"
        "  restart <name>         Controlled stop, then start\n"
        "  pause <name>           SIGSTOP\n"
        "  resume <name>          SIGCONT\n"
        "  kill <name>            SIGKILL\n"
        "  info <name>            Detailed process /proc information\n"
        "  log [name]             Tail supervisor log or a service .out file\n"
        "  reload                 Re-read config for restart policy fields\n"
        "  help                   Show this help\n"
        "  quit                   Controlled shutdown of every child\n\n"
    );
}

static void trim_inplace(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static service_t *require_service(const char *arg) {
    service_t *svc = find_service_by_name(arg);
    if (!svc) {
        printf("unknown service: %s\n", arg);
    }
    return svc;
}

void restart_service(service_t *svc) {
    if (svc->pid > 0) {
        stop_service(svc, SIGTERM);
        for (int i = 0; i < svc->stop_timeout_sec * 10 + 5 && svc->pid > 0; i++) {
            usleep(100000);
            reap_children();
            enforce_stop_timeouts();
        }
        if (svc->pid > 0) {
            kill_service(svc);
            usleep(100000);
            reap_children();
        }
    }
    svc->restart_count = 0;
    svc->total_restarts++;
    svc->next_restart_at = 0;
    start_service(svc);
}

void handle_reload(void) {
    supervisor_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    if (load_config(g_supervisor.config_path, &fresh) != 0) {
        log_event("ERROR", "Reload failed; keeping current configuration");
        return;
    }
    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *live = &g_supervisor.services[i];
        service_t *upd = NULL;
        for (int j = 0; j < fresh.count; j++) {
            if (strcmp(fresh.services[j].name, live->name) == 0) {
                upd = &fresh.services[j];
                break;
            }
        }
        if (!upd) {
            continue;
        }
        live->autorestart = upd->autorestart;
        live->max_restarts = upd->max_restarts;
        live->restart_delay_sec = upd->restart_delay_sec;
        live->backoff = upd->backoff;
        live->stop_timeout_sec = upd->stop_timeout_sec;
        live->heartbeat_timeout_sec = upd->heartbeat_timeout_sec;
        strncpy(live->command, upd->command, MAX_CMD - 1); parse_command(live); strncpy(live->workdir, upd->workdir, MAX_PATH - 1); strncpy(live->depends_on, upd->depends_on, MAX_NAME - 1); live->nice_value = upd->nice_value; live->heartbeat_enabled = upd->heartbeat_enabled; live->start_secs = upd->start_secs;
    }
    log_event("INFO", "Reloaded restart policy from %s", g_supervisor.config_path);
}

static void handle_single_command(char *cmd, char *arg) {
    if (!cmd || cmd[0] == '\0') {
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        print_status();
        return;
    }
    if (strcmp(cmd, "y") == 0) {
        start_waiting_services();
        return;
    }
    if (strcmp(cmd, "watch") == 0) {
        printf("Live status. Press Enter to return to the prompt.\n");
        g_supervisor.quiet_console = 1;
        return;
    }
    if (strcmp(cmd, "start") == 0) {
        if (!arg) {
            printf("usage: start <name|all>\n");
            return;
        }
        if (strcmp(arg, "all") == 0) {
            start_all();
            return;
        }
        service_t *svc = require_service(arg);
        if (svc) {
            start_service(svc);
        }
        return;
    }
    if (strcmp(cmd, "stop") == 0 && arg && strcmp(arg, "all") == 0) {
        for (int i = 0; i < g_supervisor.count; i++) {
            stop_service(&g_supervisor.services[i], SIGTERM);
        }
        return;
    }
    if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "pause") == 0 ||
        strcmp(cmd, "resume") == 0 || strcmp(cmd, "kill") == 0 ||
        strcmp(cmd, "restart") == 0 || strcmp(cmd, "info") == 0) {
        if (!arg) {
            printf("usage: %s <name>\n", cmd);
            return;
        }
        service_t *svc = require_service(arg);
        if (!svc) {
            return;
        }
        if (strcmp(cmd, "stop") == 0) {
            stop_service(svc, SIGTERM);
        } else if (strcmp(cmd, "pause") == 0) {
            pause_service(svc);
        } else if (strcmp(cmd, "resume") == 0) {
            resume_service(svc);
        } else if (strcmp(cmd, "kill") == 0) {
            kill_service(svc);
        } else if (strcmp(cmd, "info") == 0) {
            print_service_info(svc);
        } else {
            restart_service(svc);
        }
        return;
    }
    if (strcmp(cmd, "log") == 0) {
        print_recent_log(arg, 20);
        return;
    }
    if (strcmp(cmd, "reload") == 0) {
        handle_reload();
        return;
    }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 ||
        strcmp(cmd, "shutdown") == 0) {
        g_shutdown_requested = 1;
        return;
    }

    printf("unknown command: %s  (type help)\n", cmd);
}

static void handle_line(char *line) {
    trim_inplace(line);
    if (line[0] == '\0') {
        return;
    }

    char *save = NULL;
    for (char *segment = strtok_r(line, "\r\n", &save); segment != NULL;
         segment = strtok_r(NULL, "\r\n", &save)) {
        char *trimmed = segment;
        while (*trimmed && isspace((unsigned char)*trimmed)) {
            trimmed++;
        }
        if (*trimmed == '\0') {
            continue;
        }

        char *seg_save = NULL;
        char *cmd = strtok_r(trimmed, " \t", &seg_save);
        char *arg = strtok_r(NULL, " \t", &seg_save);
        handle_single_command(cmd, arg);
    }
}

static void tick(void) {
    if (g_child_exited) {
        g_child_exited = 0;
        reap_children();
    }
    if (g_reload_requested) {
        g_reload_requested = 0;
        handle_reload();
    }
    process_restarts();
    refresh_service_states();
    check_health();
    enforce_stop_timeouts();
}

static void event_loop(int enable_stdin) {
    int watching = 0;
    int stdin_open = enable_stdin;
    if (stdin_open && isatty(STDIN_FILENO) && !g_supervisor.quiet_console) {
        printf("supervisor> ");
        fflush(stdout);
    }

    while (g_supervisor.running) {
        if (g_shutdown_requested) {
            g_supervisor.quiet_console = 0;
            shutdown_all();
            break;
        }

        tick();

        if (watching) {
            printf("\033[2J\033[H");
            print_status();
            printf("watch mode — press Enter to return to the prompt\n");
            fflush(stdout);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rfds);
            maxfd = STDIN_FILENO;
        }
        add_heartbeat_fds(&rfds, &maxfd);
        http_server_add_fds(&rfds, &maxfd);

        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 250000;
        /* 250ms tick timeout */

        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_event("ERROR", "select() failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) {
            continue;
        }

        drain_heartbeats(&rfds);
        http_server_process(&rfds);

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[MAX_LINE];
            if (!fgets(line, sizeof(line), stdin)) {
                if (isatty(STDIN_FILENO) || g_supervisor.http_fd < 0) {
                    g_shutdown_requested = 1;
                } else {
                    stdin_open = 0;
                }
                continue;
            }
            if (watching) {
                watching = 0;
                g_supervisor.quiet_console = 0;
                printf("supervisor> ");
                fflush(stdout);
                continue;
            }
            int want_watch = (strncmp(line, "watch", 5) == 0);
            handle_line(line);
            if (want_watch && !g_shutdown_requested) {
                watching = 1;
                continue;
            }
            if (g_supervisor.running && !g_shutdown_requested && isatty(STDIN_FILENO) && !g_supervisor.quiet_console) {
                printf("supervisor> ");
                fflush(stdout);
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *config = DEFAULT_CONFIG_PATH;
    int autostart = 1;
    int port = 8080;
    const char *web_root = "linux-supervisor-website";
    int daemon_mode = 0;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--web-root") == 0) && i + 1 < argc) {
            web_root = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--no-start") == 0) {
            autostart = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-c config/services.conf] [-p 8080] [-w linux-supervisor-website] [-d|--daemon] [--no-start]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    memset(&g_supervisor, 0, sizeof(g_supervisor));
    srand((unsigned)(time(NULL) ^ (unsigned)getpid())); setvbuf(stdin, NULL, _IONBF, 0); g_supervisor.lock_fd = -1;
    g_supervisor.http_fd = -1;
    g_supervisor.http_port = port;
    if (daemon_mode) {
        g_supervisor.quiet_console = 1;
    }
    strncpy(g_supervisor.web_root, web_root, sizeof(g_supervisor.web_root) - 1);
    strncpy(g_supervisor.log_path, DEFAULT_LOG_PATH, MAX_PATH - 1);
    mkdir("logs", 0755);

    if (autostart && acquire_instance_lock(DEFAULT_LOCK_PATH) != 0) {
        return 1;
    }
    if (logger_init(g_supervisor.log_path) != 0) {
        release_instance_lock();
        return 1;
    }

    if (autostart && http_server_init(port, web_root) != 0) {
        log_event("WARN", "HTTP server initialization skipped or failed on port %d", port);
    }

    print_banner();
    install_signal_handlers();

    if (load_config(config, &g_supervisor) != 0) {
        http_server_close();
        logger_close();
        release_instance_lock();
        return 1;
    }

    g_supervisor.running = 1;
    if (autostart) {
        start_all();
    }
    if (!daemon_mode) {
        print_status();
        print_help();
    }
    event_loop(!daemon_mode);

    http_server_close();
    logger_close();
    release_instance_lock();
    return 0;
}
