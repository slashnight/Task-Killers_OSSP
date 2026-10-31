#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#endif
#include <sys/stat.h>

#include "../include/supervisor.h"

#define MAX_HTTP_CLIENTS 16
#define HTTP_REQ_BUF_SIZE 16384
#define HTTP_RESP_BUF_SIZE 262144

typedef struct {
    int fd;
    char buf[HTTP_REQ_BUF_SIZE];
    size_t len;
    time_t connected_at;
} http_client_t;

static int g_http_listen_fd = -1;
static http_client_t g_clients[MAX_HTTP_CLIENTS];

static void json_escape(const char *in, char *out, size_t out_max) {
    if (!in || !out || out_max < 2) {
        if (out && out_max > 0) out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < out_max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"') {
            if (j + 2 >= out_max) break;
            out[j++] = '\\';
            out[j++] = '"';
        } else if (c == '\\') {
            if (j + 2 >= out_max) break;
            out[j++] = '\\';
            out[j++] = '\\';
        } else if (c == '\n') {
            if (j + 2 >= out_max) break;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= out_max) break;
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= out_max) break;
            out[j++] = '\\';
            out[j++] = 't';
        } else if (c < 32) {
            /* ignore unprintable control characters */
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static long get_proc_rss_kb(pid_t pid) {
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[256];
    long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            char *p = line + 6;
            while (*p && isspace((unsigned char)*p)) p++;
            rss = atol(p);
            break;
        }
    }
    fclose(fp);
    return rss;
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcmp(dot, ".md") == 0) return "text/markdown; charset=utf-8";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

static void write_fully(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, p + total, len - total);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            break;
        }
        total += (size_t)w;
    }
}

static void send_http_response(int fd, int status_code, const char *status_text,
                               const char *content_type, const char *body, size_t body_len) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Server: LSPS-Embedded-C/1.0\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type\r\n"
                        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                        "Pragma: no-cache\r\n"
                        "Expires: 0\r\n"
                        "Connection: close\r\n\r\n",
                        status_code, status_text, content_type, body_len);

    if (hlen > 0) {
        write_fully(fd, header, (size_t)hlen);
    }
    if (body && body_len > 0) {
        write_fully(fd, body, body_len);
    }
}

static void send_cors_options(int fd) {
    const char *resp =
        "HTTP/1.1 204 No Content\r\n"
        "Server: LSPS-Embedded-C/1.0\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    write_fully(fd, resp, strlen(resp));
}

static void handle_api_status(int fd) {
    char *json = malloc(HTTP_RESP_BUF_SIZE);
    if (!json) {
        send_http_response(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"out of memory\"}", 25);
        return;
    }

    struct rusage self_ru, ch_ru;
    memset(&self_ru, 0, sizeof(self_ru));
    memset(&ch_ru, 0, sizeof(ch_ru));
    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &ch_ru);

    double loadavg[3] = {0.0, 0.0, 0.0};
    getloadavg(loadavg, 3);

    int running_count = 0;
    int failed_count = 0;
    int waiting_count = 0;
    int total_restarts = 0;
    int total_crashes = 0;

    for (int i = 0; i < g_supervisor.count; i++) {
        service_t *s = &g_supervisor.services[i];
        if (s->state == STATE_RUNNING || s->state == STATE_PAUSED) running_count++;
        else if (s->state == STATE_FAILED || s->state == STATE_UNHEALTHY) failed_count++;
        else if (s->state == STATE_WAITING) waiting_count++;
        total_restarts += s->total_restarts;
        total_crashes += s->crash_count;
    }

    time_t now = time(NULL);
    long sup_rss = get_proc_rss_kb(getpid());

    int offset = snprintf(json, HTTP_RESP_BUF_SIZE,
        "{\n"
        "  \"supervisor\": {\n"
        "    \"pid\": %d,\n"
        "    \"ppid\": %d,\n"
        "    \"running\": %d,\n"
        "    \"http_port\": %d,\n"
        "    \"total_services\": %d,\n"
        "    \"running_services\": %d,\n"
        "    \"failed_services\": %d,\n"
        "    \"waiting_services\": %d,\n"
        "    \"total_restarts\": %d,\n"
        "    \"total_crashes\": %d,\n"
        "    \"load_avg\": [%.2f, %.2f, %.2f],\n"
        "    \"resources\": {\n"
        "      \"self_user_cpu\": %ld.%06ld,\n"
        "      \"self_sys_cpu\": %ld.%06ld,\n"
        "      \"children_user_cpu\": %ld.%06ld,\n"
        "      \"children_sys_cpu\": %ld.%06ld,\n"
        "      \"children_max_rss_kb\": %ld,\n"
        "      \"supervisor_rss_kb\": %ld\n"
        "    }\n"
        "  },\n"
        "  \"services\": [\n",
        (int)getpid(), (int)getppid(), g_supervisor.running,
        g_supervisor.http_port, g_supervisor.count,
        running_count, failed_count, waiting_count,
        total_restarts, total_crashes,
        loadavg[0], loadavg[1], loadavg[2],
        (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec,
        (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec,
        (long)ch_ru.ru_utime.tv_sec, (long)ch_ru.ru_utime.tv_usec,
        (long)ch_ru.ru_stime.tv_sec, (long)ch_ru.ru_stime.tv_usec,
        (long)ch_ru.ru_maxrss, sup_rss
    );

    for (int i = 0; i < g_supervisor.count && offset < HTTP_RESP_BUF_SIZE - 2048; i++) {
        service_t *svc = &g_supervisor.services[i];
        long uptime = 0;
        if (svc->state == STATE_RUNNING && svc->start_time > 0) {
            uptime = (long)(now - svc->start_time);
        }
        long last_hb_ago = -1;
        if (svc->heartbeat_enabled && svc->last_heartbeat > 0) {
            last_hb_ago = (long)(now - svc->last_heartbeat);
        }
        long rss = get_proc_rss_kb(svc->pid);

        char esc_cmd[MAX_CMD * 2];
        char esc_workdir[MAX_PATH * 2];
        json_escape(svc->command, esc_cmd, sizeof(esc_cmd));
        json_escape(svc->workdir, esc_workdir, sizeof(esc_workdir));

        offset += snprintf(json + offset, HTTP_RESP_BUF_SIZE - (size_t)offset,
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"command\": \"%s\",\n"
            "      \"workdir\": \"%s\",\n"
            "      \"depends_on\": \"%s\",\n"
            "      \"pid\": %d,\n"
            "      \"ppid\": %d,\n"
            "      \"pgid\": %d,\n"
            "      \"state\": \"%s\",\n"
            "      \"state_id\": %d,\n"
            "      \"stable\": %d,\n"
            "      \"uptime_sec\": %ld,\n"
            "      \"restart_count\": %d,\n"
            "      \"total_restarts\": %d,\n"
            "      \"max_restarts\": %d,\n"
            "      \"crash_count\": %d,\n"
            "      \"start_count\": %d,\n"
            "      \"stop_count\": %d,\n"
            "      \"last_exit_status\": %d,\n"
            "      \"last_term_signal\": %d,\n"
            "      \"autorestart\": %d,\n"
            "      \"backoff\": %d,\n"
            "      \"delay_sec\": %d,\n"
            "      \"stop_timeout_sec\": %d,\n"
            "      \"start_secs\": %d,\n"
            "      \"nice\": %d,\n"
            "      \"heartbeat_enabled\": %d,\n"
            "      \"heartbeat_timeout_sec\": %d,\n"
            "      \"last_heartbeat_ago\": %ld,\n"
            "      \"waiting_notified\": %d,\n"
            "      \"rss_kb\": %ld\n"
            "    }%s\n",
            svc->name, esc_cmd, esc_workdir, svc->depends_on,
            (int)svc->pid, (svc->pid > 0 ? (int)getpid() : -1), (int)svc->pgid,
            state_to_string(svc->state), (int)svc->state,
            svc->stable, uptime, svc->restart_count, svc->total_restarts, svc->max_restarts,
            svc->crash_count, svc->start_count, svc->stop_count,
            svc->last_exit_status, svc->last_term_signal,
            svc->autorestart, svc->backoff, svc->restart_delay_sec,
            svc->stop_timeout_sec, svc->start_secs, svc->nice_value,
            svc->heartbeat_enabled, svc->heartbeat_timeout_sec, last_hb_ago,
            svc->waiting_notified, rss,
            (i + 1 < g_supervisor.count ? "," : "")
        );
    }

    offset += snprintf(json + offset, HTTP_RESP_BUF_SIZE - (size_t)offset, "  ]\n}\n");

    send_http_response(fd, 200, "OK", "application/json", json, (size_t)offset);
    free(json);
}

static void extract_param(const char *src, const char *key, char *out, size_t out_max) {
    out[0] = '\0';
    if (!src || !key || out_max < 1) return;

    const char *p = strstr(src, key);
    if (!p) return;

    p += strlen(key);

    while (*p && (*p == '"' || *p == '\\' || *p == ':' || *p == '=' || *p == ' ' || *p == '\'')) {
        p++;
    }

    size_t j = 0;
    while (*p && *p != '"' && *p != '\'' && *p != '\\' && *p != ',' && *p != '}' && *p != '&' && *p != '\r' && *p != '\n' && j + 1 < out_max) {
        out[j++] = *p++;
    }
    out[j] = '\0';

    while (j > 0 && isspace((unsigned char)out[j - 1])) {
        out[--j] = '\0';
    }
}

static void handle_api_action(int fd, const char *body, const char *query, const char *default_action) {
    char action[64] = "";
    char target[64] = "";

    extract_param(body, "action", action, sizeof(action));
    if (action[0] == '\0') {
        extract_param(query, "action", action, sizeof(action));
    }
    if (action[0] == '\0' && default_action) {
        strncpy(action, default_action, sizeof(action) - 1);
    }
    extract_param(body, "service", target, sizeof(target));
    if (target[0] == '\0') {
        extract_param(query, "service", target, sizeof(target));
    }

    log_event("DEBUG", "handle_api_action: body='%s', action='%s', target='%s'", body ? body : "(null)", action, target);

    if (action[0] == '\0') {
        send_http_response(fd, 400, "Bad Request", "application/json",
                           "{\"success\":false,\"message\":\"Missing 'action' parameter\"}", 52);
        return;
    }

    char resp[512];
    int success = 1;
    char message[256] = "";

    if (strcmp(action, "start_all") == 0) {
        int started = start_all();
        snprintf(message, sizeof(message), "Started %d service(s)", started);
    } else if (strcmp(action, "stop_all") == 0) {
        int stopped = 0;
        for (int i = 0; i < g_supervisor.count; i++) {
            stop_service(&g_supervisor.services[i], SIGTERM);
            stopped++;
        }
        snprintf(message, sizeof(message), "Stopped %d service(s)", stopped);
    } else if (strcmp(action, "approve_waiting") == 0 || strcmp(action, "y") == 0) {
        int approved = start_waiting_services();
        snprintf(message, sizeof(message), "Started %d ready waiting service(s)", approved);
    } else if (strcmp(action, "reload") == 0) {
        handle_reload();
        snprintf(message, sizeof(message), "Configuration reloaded successfully");
    } else if (strcmp(action, "shutdown") == 0) {
        g_shutdown_requested = 1;
        snprintf(message, sizeof(message), "Supervisor shutdown initiated");
    } else if (strcmp(action, "random_restart") == 0) {
        int r_indices[MAX_SERVICES];
        int r_count = 0;
        for (int i = 0; i < g_supervisor.count; i++) {
            if (g_supervisor.services[i].state == STATE_RUNNING && g_supervisor.services[i].pid > 0) {
                r_indices[r_count++] = i;
            }
        }
        if (r_count > 0) {
            int pick = r_indices[rand() % r_count];
            service_t *chosen = &g_supervisor.services[pick];
            snprintf(target, sizeof(target), "%s", chosen->name);
            restart_service(chosen);
            snprintf(message, sizeof(message), "Random chaos restart executed on service '%s'", chosen->name);
        } else {
            snprintf(message, sizeof(message), "No running services to restart");
            success = 0;
        }
    } else {
        if (target[0] == '\0') {
            send_http_response(fd, 400, "Bad Request", "application/json",
                               "{\"success\":false,\"message\":\"Missing 'service' parameter\"}", 53);
            return;
        }
        service_t *svc = find_service_by_name(target);
        if (!svc) {
            snprintf(message, sizeof(message), "Unknown service '%s'", target);
            success = 0;
        } else {
            if (strcmp(action, "start") == 0) {
                int rc = start_service(svc);
                if (rc == 0) snprintf(message, sizeof(message), "Service '%s' started", target);
                else if (rc == 1) snprintf(message, sizeof(message), "Service '%s' placed in WAITING on dependency", target);
                else { snprintf(message, sizeof(message), "Failed to start service '%s'", target); success = 0; }
            } else if (strcmp(action, "stop") == 0) {
                int rc = stop_service(svc, SIGTERM);
                if (rc == 0) snprintf(message, sizeof(message), "SIGTERM sent to '%s'", target);
                else { snprintf(message, sizeof(message), "Cannot stop '%s'", target); success = 0; }
            } else if (strcmp(action, "restart") == 0) {
                restart_service(svc);
                snprintf(message, sizeof(message), "Service '%s' restarted", target);
            } else if (strcmp(action, "pause") == 0) {
                int rc = pause_service(svc);
                if (rc == 0) snprintf(message, sizeof(message), "SIGSTOP sent to '%s'", target);
                else { snprintf(message, sizeof(message), "Cannot pause '%s' (current state: %s)", target, state_to_string(svc->state)); success = 0; }
            } else if (strcmp(action, "resume") == 0) {
                int rc = resume_service(svc);
                if (rc == 0) snprintf(message, sizeof(message), "SIGCONT sent to '%s'", target);
                else { snprintf(message, sizeof(message), "Cannot resume '%s' (current state: %s)", target, state_to_string(svc->state)); success = 0; }
            } else if (strcmp(action, "kill") == 0) {
                int rc = kill_service(svc);
                if (rc == 0) snprintf(message, sizeof(message), "SIGKILL sent to '%s'", target);
                else { snprintf(message, sizeof(message), "Cannot kill '%s'", target); success = 0; }
            } else {
                snprintf(message, sizeof(message), "Unsupported action '%s'", action);
                success = 0;
            }
        }
    }

    snprintf(resp, sizeof(resp),
             "{\"success\":%s,\"action\":\"%s\",\"service\":\"%s\",\"message\":\"%s\"}",
             success ? "true" : "false", action, target, message);
    send_http_response(fd, success ? 200 : 400, success ? "OK" : "Bad Request", "application/json", resp, strlen(resp));
}

static void handle_api_logs(int fd, const char *query) {
    char target[64] = "";
    int lines = 50;

    if (query) {
        const char *p = strstr(query, "service=");
        if (!p) p = strstr(query, "source=");
        if (p) {
            p = strchr(p, '=');
            if (p) p++;
            int j = 0;
            while (p && *p && *p != '&' && *p != ' ' && j < (int)sizeof(target) - 1) {
                target[j++] = *p++;
            }
            target[j] = '\0';
        }
        p = strstr(query, "lines=");
        if (p) {
            lines = atoi(p + 6);
            if (lines <= 0) lines = 50;
            if (lines > 300) lines = 300;
        }
    }

    char path[MAX_PATH];
    if (target[0] != '\0' && strcmp(target, "all") != 0 && strcmp(target, "supervisor") != 0) {
        snprintf(path, sizeof(path), "logs/%s.out", target);
    } else {
        snprintf(path, sizeof(path), "%s", g_supervisor.log_path);
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "{\"path\":\"%s\",\"lines\":[],\"error\":\"Cannot open file\"}", path);
        send_http_response(fd, 200, "OK", "application/json", err, strlen(err));
        return;
    }

    char **ring = calloc((size_t)lines, sizeof(char *));
    int count = 0;
    int idx = 0;
    char line_buf[MAX_LINE];

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        size_t l = strlen(line_buf);
        while (l > 0 && (line_buf[l - 1] == '\r' || line_buf[l - 1] == '\n')) {
            line_buf[--l] = '\0';
        }
        free(ring[idx]);
        ring[idx] = strdup(line_buf);
        idx = (idx + 1) % lines;
        if (count < lines) count++;
    }
    fclose(fp);

    char *json = malloc(HTTP_RESP_BUF_SIZE);
    if (!json) {
        for (int i = 0; i < lines; i++) free(ring[i]);
        free(ring);
        send_http_response(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"out of memory\"}", 25);
        return;
    }

    int offset = snprintf(json, HTTP_RESP_BUF_SIZE,
        "{\n"
        "  \"path\": \"%s\",\n"
        "  \"service\": \"%s\",\n"
        "  \"total_lines\": %d,\n"
        "  \"lines\": [\n",
        path, target, count);

    int start = (count == lines) ? idx : 0;
    for (int i = 0; i < count && offset < HTTP_RESP_BUF_SIZE - 512; i++) {
        int pos = (start + i) % lines;
        char escaped[MAX_LINE * 2];
        json_escape(ring[pos], escaped, sizeof(escaped));
        offset += snprintf(json + offset, HTTP_RESP_BUF_SIZE - (size_t)offset,
                           "    \"%s\"%s\n", escaped, (i + 1 < count ? "," : ""));
        free(ring[pos]);
    }
    free(ring);

    offset += snprintf(json + offset, HTTP_RESP_BUF_SIZE - (size_t)offset, "  ]\n}\n");
    send_http_response(fd, 200, "OK", "application/json", json, (size_t)offset);
    free(json);
}

static void handle_api_config(int fd, const char *method, const char *body) {
    if (strcmp(method, "GET") == 0) {
        FILE *fp = fopen(g_supervisor.config_path, "r");
        if (!fp) {
            send_http_response(fd, 404, "Not Found", "application/json",
                               "{\"error\":\"Config file not found\"}", 32);
            return;
        }

        char *content = malloc(65536);
        if (!content) {
            fclose(fp);
            send_http_response(fd, 500, "Server Error", "application/json", "{\"error\":\"OOM\"}", 15);
            return;
        }
        size_t n = fread(content, 1, 65535, fp);
        content[n] = '\0';
        fclose(fp);

        char *escaped = malloc(131072);
        char *json = malloc(131072);
        if (!escaped || !json) {
            free(content);
            if (escaped) free(escaped);
            if (json) free(json);
            send_http_response(fd, 500, "Server Error", "application/json", "{\"error\":\"OOM\"}", 15);
            return;
        }

        json_escape(content, escaped, 131072);
        int jlen = snprintf(json, 131072, "{\"path\":\"%s\",\"raw\":\"%s\"}", g_supervisor.config_path, escaped);
        send_http_response(fd, 200, "OK", "application/json", json, (size_t)jlen);

        free(content);
        free(escaped);
        free(json);
    } else if (strcmp(method, "POST") == 0) {
        const char *p = strstr(body, "\"raw\":");
        if (!p) {
            send_http_response(fd, 400, "Bad Request", "application/json",
                               "{\"success\":false,\"message\":\"Missing 'raw' content\"}", 49);
            return;
        }
        p += 6;
        while (*p && (*p == ' ' || *p == '"')) p++;
        /* Extract raw content and unescape \n, \", etc. */
        char *new_conf = malloc(65536);
        if (!new_conf) {
            send_http_response(fd, 500, "Server Error", "application/json", "{\"error\":\"OOM\"}", 15);
            return;
        }
        size_t ci = 0;
        while (*p && ci < 65535) {
            if (*p == '"' && *(p - 1) != '\\') break;
            if (*p == '\\') {
                p++;
                if (*p == 'n') new_conf[ci++] = '\n';
                else if (*p == 'r') new_conf[ci++] = '\r';
                else if (*p == 't') new_conf[ci++] = '\t';
                else if (*p == '"') new_conf[ci++] = '"';
                else if (*p == '\\') new_conf[ci++] = '\\';
                else if (*p) new_conf[ci++] = *p;
            } else {
                new_conf[ci++] = *p;
            }
            if (*p) p++;
        }
        new_conf[ci] = '\0';

        FILE *fp = fopen(g_supervisor.config_path, "w");
        if (!fp) {
            free(new_conf);
            send_http_response(fd, 500, "Server Error", "application/json",
                               "{\"success\":false,\"message\":\"Cannot write config\"}", 50);
            return;
        }
        fputs(new_conf, fp);
        fclose(fp);
        free(new_conf);

        handle_reload();
        send_http_response(fd, 200, "OK", "application/json",
                           "{\"success\":true,\"message\":\"Configuration saved and reloaded\"}", 62);
    }
}

static void handle_static_file(int fd, const char *url_path) {
    char rel_path[MAX_PATH * 2];
    if (strcmp(url_path, "/") == 0 || strcmp(url_path, "/index.html") == 0) {
        snprintf(rel_path, sizeof(rel_path), "%s/index.html", g_supervisor.web_root);
    } else {
        /* Disallow .. in path */
        if (strstr(url_path, "..")) {
            send_http_response(fd, 403, "Forbidden", "text/plain", "Forbidden", 9);
            return;
        }
        const char *clean_url = url_path;
        if (clean_url[0] == '/') clean_url++;
        snprintf(rel_path, sizeof(rel_path), "%s/%s", g_supervisor.web_root, clean_url);
    }

    FILE *fp = fopen(rel_path, "rb");
    if (!fp) {
        char err[512];
        int elen = snprintf(err, sizeof(err), "File not found: %s", rel_path);
        send_http_response(fd, 404, "Not Found", "text/plain", err, (size_t)elen);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0 || size > 20000000) {
        fclose(fp);
        send_http_response(fd, 500, "Internal Server Error", "text/plain", "File too large", 14);
        return;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        send_http_response(fd, 500, "Internal Server Error", "text/plain", "Out of memory", 13);
        return;
    }
    size_t read_bytes = fread(buf, 1, (size_t)size, fp);
    fclose(fp);

    const char *mime = get_mime_type(rel_path);
    send_http_response(fd, 200, "OK", mime, buf, read_bytes);
    free(buf);
}

static void process_client_request(http_client_t *c) {
    char method[16] = "";
    char url[MAX_PATH] = "";
    char *body = NULL;

    char *line_end = strstr(c->buf, "\r\n");
    if (!line_end) return;

    *line_end = '\0';
    sscanf(c->buf, "%15s %255s", method, url);
    *line_end = '\r';

    char *body_delim = strstr(c->buf, "\r\n\r\n");
    if (body_delim) {
        body = body_delim + 4;
    }

    char *query = strchr(url, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        send_cors_options(c->fd);
    } else if (strcmp(url, "/api/status") == 0) {
        handle_api_status(c->fd);
    } else if (strcmp(url, "/api/action") == 0) {
        handle_api_action(c->fd, body, query, NULL);
    } else if (strcmp(url, "/api/restart") == 0) {
        handle_api_action(c->fd, body, query, "restart");
    } else if (strcmp(url, "/api/start") == 0) {
        handle_api_action(c->fd, body, query, "start");
    } else if (strcmp(url, "/api/stop") == 0) {
        handle_api_action(c->fd, body, query, "stop");
    } else if (strcmp(url, "/api/logs") == 0) {
        handle_api_logs(c->fd, query);
    } else if (strcmp(url, "/api/config") == 0) {
        handle_api_config(c->fd, method, body);
    } else {
        handle_static_file(c->fd, url);
    }
}

int http_server_init(int port, const char *web_root) {
    if (port <= 0) port = 8080;
    if (!web_root) web_root = "linux-supervisor-website";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("http socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_event("WARN", "HTTP bind failed on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 32) < 0) {
        log_event("WARN", "HTTP listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_http_listen_fd = fd;
    g_supervisor.http_fd = fd;
    g_supervisor.http_port = port;
    strncpy(g_supervisor.web_root, web_root, sizeof(g_supervisor.web_root) - 1);

    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].len = 0;
    }

    log_event("INFO", "HTTP Server listening on 0.0.0.0:%d (web root: %s)", port, web_root);
    return 0;
}

void http_server_add_fds(fd_set *rfds, int *maxfd) {
    if (g_http_listen_fd >= 0) {
        FD_SET(g_http_listen_fd, rfds);
        if (g_http_listen_fd > *maxfd) {
            *maxfd = g_http_listen_fd;
        }
    }
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            FD_SET(g_clients[i].fd, rfds);
            if (g_clients[i].fd > *maxfd) {
                *maxfd = g_clients[i].fd;
            }
        }
    }
}

void http_server_process(fd_set *rfds) {
    time_t now = time(NULL);

    if (g_http_listen_fd >= 0 && FD_ISSET(g_http_listen_fd, rfds)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(g_http_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (cfd >= 0) {
            int flags = fcntl(cfd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
            }
            int slot = -1;
            for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                if (g_clients[i].fd < 0) {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                g_clients[slot].fd = cfd;
                g_clients[slot].len = 0;
                g_clients[slot].connected_at = now;
            } else {
                close(cfd);
            }
        }
    }

    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_clients[i].fd < 0) continue;

        if (now - g_clients[i].connected_at > 10) {
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_clients[i].len = 0;
            continue;
        }

        if (FD_ISSET(g_clients[i].fd, rfds)) {
            ssize_t n = read(g_clients[i].fd,
                             g_clients[i].buf + g_clients[i].len,
                             sizeof(g_clients[i].buf) - 1 - g_clients[i].len);
            if (n > 0) {
                g_clients[i].len += (size_t)n;
                g_clients[i].buf[g_clients[i].len] = '\0';

                char *hdr_end = strstr(g_clients[i].buf, "\r\n\r\n");
                if (hdr_end) {
                    size_t header_len = (size_t)(hdr_end - g_clients[i].buf) + 4;
                    size_t content_len = 0;
                    const char *cl = strstr(g_clients[i].buf, "Content-Length:");
                    if (!cl) cl = strstr(g_clients[i].buf, "content-length:");
                    if (cl && cl < hdr_end) {
                        content_len = (size_t)atol(cl + 15);
                    }
                    if (g_clients[i].len >= header_len + content_len) {
                        process_client_request(&g_clients[i]);
                        close(g_clients[i].fd);
                        g_clients[i].fd = -1;
                        g_clients[i].len = 0;
                    }
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(g_clients[i].fd);
                g_clients[i].fd = -1;
                g_clients[i].len = 0;
            }
        }
    }
}

void http_server_close(void) {
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_clients[i].len = 0;
        }
    }
    if (g_http_listen_fd >= 0) {
        close(g_http_listen_fd);
        g_http_listen_fd = -1;
        g_supervisor.http_fd = -1;
    }
}
