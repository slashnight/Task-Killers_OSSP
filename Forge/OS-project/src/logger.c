#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../include/supervisor.h"

static int g_log_fd = -1;

static void write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t written = write(fd, buf + total, len - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write");
            break;
        }
        total += (size_t)written;
    }
}

int logger_init(const char *path) {
    if (!path) {
        return -1;
    }

    g_log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log_fd < 0) {
        perror("open log file");
        return -1;
    }

    log_event("INFO", "Supervisor logger started for pid=%d path=%s", (int)getpid(), path);
    return 0;
}

void logger_close(void) {
    if (g_log_fd >= 0) {
        log_event("INFO", "Supervisor logger closed");
        close(g_log_fd);
        g_log_fd = -1;
    }
}

void log_event(const char *level, const char *fmt, ...) {
    char buffer[1024];
    char timebuf[32];
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char entry[1200];
    snprintf(entry, sizeof(entry), "[%s] [%s] [PID: %d] %s\n",
             timebuf, level, (int)getpid(), buffer);

    if (g_log_fd >= 0) {
        write_all(g_log_fd, entry, strlen(entry));
    }

    if (!g_supervisor.quiet_console) {
        printf("%s", entry);
        fflush(stdout);
    }
}

void log_rusage(const char *service_name) {
    struct rusage usage;
    if (getrusage(RUSAGE_CHILDREN, &usage) != 0) {
        perror("getrusage");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Resource summary for %s: user_cpu=%ld.%06ld s system_cpu=%ld.%06ld s maxrss=%ld KB",
             service_name ? service_name : "all",
             (long)usage.ru_utime.tv_sec, (long)usage.ru_utime.tv_usec,
             (long)usage.ru_stime.tv_sec, (long)usage.ru_stime.tv_usec,
             usage.ru_maxrss);
    log_event("INFO", "%s", msg);
}
