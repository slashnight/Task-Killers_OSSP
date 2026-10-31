#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>

#define MAX_SERVICES          32
#define MAX_NAME              64
#define MAX_CMD               512
#define MAX_ARGS              24
#define MAX_ARG_LEN           128
#define MAX_PATH              256
#define MAX_LINE              512
#define MAX_BACKOFF_SEC       16
#define HEARTBEAT_CHILD_FD    3
#define DEFAULT_LOG_PATH      "logs/supervisor.log"
#define DEFAULT_CONFIG_PATH   "config/services.conf"
#define DEFAULT_LOCK_PATH     "logs/supervisor.lock"

typedef enum {
    STATE_STOPPED = 0,
    STATE_WAITING,   /* blocked on a dependency */
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_STOPPING,
    STATE_FAILED,
    STATE_UNHEALTHY
} service_state_t;

typedef struct {
    char name[MAX_NAME];
    char command[MAX_CMD];
    char workdir[MAX_PATH];
    char depends_on[MAX_NAME];
    char argv_storage[MAX_ARGS][MAX_ARG_LEN];
    char *argv[MAX_ARGS + 1];
    int argc;

    int autorestart;
    int max_restarts;
    int restart_delay_sec;
    int backoff;
    int stop_timeout_sec;
    int start_secs;
    int nice_value;
    int heartbeat_enabled;
    int heartbeat_timeout_sec;

    pid_t pid;
    pid_t pgid;
    int hb_fd;
    service_state_t state;
    int restart_count;
    int crash_count;
    int total_restarts;
    int expected_stop;
    int stable;
    int last_exit_status;
    int last_term_signal;
    int start_count;
    int stop_count;
    int waiting_notified;
    time_t start_time;
    time_t last_start_time;
    time_t last_stop_time;
    time_t last_exit_time;
    time_t last_heartbeat;
    time_t next_restart_at;
    time_t stop_deadline;
} service_t;

typedef struct {
    service_t services[MAX_SERVICES];
    int count;
    int running;
    int quiet_console;
    int lock_fd;
    char config_path[MAX_PATH];
    char log_path[MAX_PATH];
    int http_port;
    int http_fd;
    char web_root[MAX_PATH];
} supervisor_t;

extern supervisor_t g_supervisor;
extern volatile sig_atomic_t g_child_exited;
extern volatile sig_atomic_t g_shutdown_requested;
extern volatile sig_atomic_t g_reload_requested;

const char *state_to_string(service_state_t state);
service_t *find_service_by_name(const char *name);
service_t *find_service_by_pid(pid_t pid);

int logger_init(const char *path);
void log_event(const char *level, const char *fmt, ...);
void log_rusage(const char *service_name);
void logger_close(void);

int load_config(const char *path, supervisor_t *sv);
void parse_command(service_t *svc);

void install_signal_handlers(void);

int start_service(service_t *svc);
int stop_service(service_t *svc, int sig);
int pause_service(service_t *svc);
int resume_service(service_t *svc);
int kill_service(service_t *svc);
int start_all(void);
void reap_children(void);
void process_restarts(void);
void refresh_service_states(void);
int start_waiting_services(void);
void check_health(void);
void enforce_stop_timeouts(void);
void drain_heartbeats(fd_set *rfds);
void add_heartbeat_fds(fd_set *rfds, int *maxfd);
void print_status(void);
void print_service_info(const service_t *svc);
void print_recent_log(const char *name, int lines);
void shutdown_all(void);
void print_final_summary(void);
void close_service_fds(service_t *svc);
int acquire_instance_lock(const char *path);
void release_instance_lock(void);

void restart_service(service_t *svc);
void handle_reload(void);

int http_server_init(int port, const char *web_root);
void http_server_add_fds(fd_set *rfds, int *maxfd);
void http_server_process(fd_set *rfds);
void http_server_close(void);

#endif
