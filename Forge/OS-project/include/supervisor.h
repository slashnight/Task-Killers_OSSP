#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <signal.h>
#ifndef _WIN32
#include <sys/select.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <string.h>
#include <time.h>

#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 1
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif
#ifndef WCONTINUED
#define WCONTINUED 8
#endif
#ifndef WIFEXITED
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#endif
#ifndef WTERMSIG
#define WTERMSIG(s) ((s) & 0x7f)
#endif
#ifndef WIFSTOPPED
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#endif
#ifndef WIFCONTINUED
#define WIFCONTINUED(s) ((s) == 0xffff)
#endif

#ifndef F_GETFD
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define O_NONBLOCK 0x0004
#define O_CLOEXEC 0x80000
#endif

#ifndef LOCK_EX
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif

#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

#ifndef RUSAGE_SELF
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nvcsw;
    long ru_nivcsw;
};
#endif

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX 4
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif
#ifdef MAX_PATH
#undef MAX_PATH
#endif
#include <stdint.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

typedef int sigset_t;
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
};

static inline int sigemptyset(sigset_t *s) { if (s) *s = 0; return 0; }
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) { (void)sig; (void)act; (void)oact; return 0; }
static inline pid_t waitpid(pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; return -1; }
static inline pid_t fork(void) { return -1; }
static inline int pipe(int fds[2]) { (void)fds; return -1; }
static inline pid_t setsid(void) { return 0; }
static inline pid_t getppid(void) { return 0; }
static inline int kill(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
static inline int killpg(pid_t pgrp, int sig) { (void)pgrp; (void)sig; return 0; }
static inline int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static inline int flock(int fd, int op) { (void)fd; (void)op; return 0; }
static inline int setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
static inline int getrusage(int who, struct rusage *u) { if (u) memset(u, 0, sizeof(*u)); (void)who; return 0; }
static inline long sysconf(int name) { (void)name; return 1; }
static inline const char *strsignal(int sig) { (void)sig; return "SIGNAL"; }
static inline int setenv(const char *name, const char *value, int overwrite) { (void)overwrite; return _putenv_s(name, value); }
static inline int getloadavg(double loadavg[], int nelem) { for (int i = 0; i < nelem; ++i) loadavg[i] = 0.0; return nelem; }
#ifndef localtime_r
static inline struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return localtime_s(result, timep) == 0 ? result : NULL;
}
#endif
#ifndef strtok_r
#define strtok_r strtok_s
#endif
#define mkdir(path, mode) _mkdir(path)
#endif
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
