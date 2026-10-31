#include <signal.h>
#include <string.h>

#include "supervisor.h"

volatile sig_atomic_t g_child_exited = 0;
volatile sig_atomic_t g_shutdown_requested = 0;
volatile sig_atomic_t g_reload_requested = 0;

static void on_sigchld(int signo) {
    (void)signo;
    g_child_exited = 1;
}

static void on_shutdown(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static void on_hup(int signo) {
    (void)signo;
    g_reload_requested = 1;
}

void install_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_hup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}
