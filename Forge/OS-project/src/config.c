#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>

#include "../include/supervisor.h"

static void trim(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static int truthy(const char *value) {
    return strcmp(value, "yes") == 0 || strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

static service_t *find_service_in(supervisor_t *sv, const char *name) {
    if (!sv || !name) {
        return NULL;
    }
    for (int i = 0; i < sv->count; i++) {
        if (strcmp(sv->services[i].name, name) == 0) {
            return &sv->services[i];
        }
    }
    return NULL;
}

static void reset_service_defaults(service_t *svc) {
    memset(svc, 0, sizeof(*svc));
    svc->autorestart = 1;
    svc->max_restarts = 5;
    svc->restart_delay_sec = 2;
    svc->backoff = 1;
    svc->stop_timeout_sec = 4;
    svc->start_secs = 2;
    svc->nice_value = 0;
    svc->heartbeat_timeout_sec = 6;
    svc->pid = -1;
    svc->pgid = -1;
    svc->hb_fd = -1;
    svc->state = STATE_STOPPED;
}

void parse_command(service_t *svc) {
    svc->argc = 0;
    char buf[MAX_CMD];
    strncpy(buf, svc->command, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);
    while (tok && svc->argc < MAX_ARGS) {
        strncpy(svc->argv_storage[svc->argc], tok, MAX_ARG_LEN - 1);
        svc->argv_storage[svc->argc][MAX_ARG_LEN - 1] = '\0';
        svc->argv[svc->argc] = svc->argv_storage[svc->argc];
        svc->argc++;
        tok = strtok_r(NULL, " \t", &save);
    }
    svc->argv[svc->argc] = NULL;
}

static int apply_key(service_t *svc, const char *key, const char *value) {
    if (strcmp(key, "command") == 0) {
        strncpy(svc->command, value, MAX_CMD - 1);
        return 0;
    }
    if (strcmp(key, "workdir") == 0) {
        strncpy(svc->workdir, value, MAX_PATH - 1);
        return 0;
    }
    if (strcmp(key, "depends_on") == 0) {
        strncpy(svc->depends_on, value, MAX_NAME - 1);
        return 0;
    }
    if (strcmp(key, "autorestart") == 0) {
        svc->autorestart = truthy(value);
        return 0;
    }
    if (strcmp(key, "max_restarts") == 0) {
        svc->max_restarts = atoi(value);
        return 0;
    }
    if (strcmp(key, "delay") == 0 || strcmp(key, "restart_delay") == 0) {
        svc->restart_delay_sec = atoi(value);
        return 0;
    }
    if (strcmp(key, "backoff") == 0) {
        svc->backoff = truthy(value);
        return 0;
    }
    if (strcmp(key, "stop_timeout") == 0) {
        svc->stop_timeout_sec = atoi(value);
        return 0;
    }
    if (strcmp(key, "start_secs") == 0) {
        svc->start_secs = atoi(value);
        return 0;
    }
    if (strcmp(key, "nice") == 0) {
        svc->nice_value = atoi(value);
        return 0;
    }
    if (strcmp(key, "heartbeat") == 0) {
        svc->heartbeat_enabled = truthy(value);
        return 0;
    }
    if (strcmp(key, "heartbeat_timeout") == 0) {
        svc->heartbeat_timeout_sec = atoi(value);
        return 0;
    }
    return -1;
}

int load_config(const char *path, supervisor_t *sv) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("fopen config");
        return -1;
    }

    sv->count = 0;
    service_t *cur = NULL;
    char line[MAX_LINE];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) {
                fprintf(stderr, "config:%d: missing ']'\n", lineno);
                fclose(fp);
                return -1;
            }
            *end = '\0';
            if (sv->count >= MAX_SERVICES) {
                fprintf(stderr, "config:%d: too many services\n", lineno);
                fclose(fp);
                return -1;
            }
            cur = &sv->services[sv->count++];
            reset_service_defaults(cur);
            if (strlen(line + 1) >= sizeof(cur->name)) {
                fprintf(stderr, "config:%d: service name is too long\n", lineno);
                fclose(fp);
                return -1;
            }
            memcpy(cur->name, line + 1, strlen(line + 1) + 1);
            continue;
        }

        if (!cur) {
            fprintf(stderr, "config:%d: key outside of a [service] section\n", lineno);
            fclose(fp);
            return -1;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "config:%d: expected key=value\n", lineno);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);
        if (apply_key(cur, key, value) != 0) {
            fprintf(stderr, "config:%d: unknown key '%s'\n", lineno, key);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    for (int i = 0; i < sv->count; i++) {
        if (sv->services[i].command[0] == '\0') {
            fprintf(stderr, "service '%s' has no command\n", sv->services[i].name);
            return -1;
        }
        parse_command(&sv->services[i]);
        if (sv->services[i].argc == 0) {
            fprintf(stderr, "service '%s' has an empty command\n", sv->services[i].name);
            return -1;
        }
        if (sv->services[i].depends_on[0] &&
            !find_service_in(sv, sv->services[i].depends_on)) {
            fprintf(stderr, "service '%s' depends on unknown '%s'\n",
                    sv->services[i].name, sv->services[i].depends_on);
            return -1;
        }
    }

    strncpy(sv->config_path, path, MAX_PATH - 1);
    log_event("INFO", "Loaded %d service(s) from %s", sv->count, path);
    return 0;
}
