#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

static volatile sig_atomic_t running = 1;

static void on_term(int signo) {
    (void)signo;
    running = 0;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "worker";
    int interval = argc > 2 ? atoi(argv[2]) : 2;
    if (interval < 1) {
        interval = 1;
    }

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int hb = -1;
    const char *fdstr = getenv("SUPERVISOR_HB_FD");
    if (fdstr) {
        hb = atoi(fdstr);
    }

    printf("[%s] worker started pid=%d heartbeat_fd=%d\n",
           name, (int)getpid(), hb);
    fflush(stdout);

    /* Seed PRNG uniquely per process instance */
    srand((unsigned)(time(NULL) ^ ((unsigned)getpid() << 16)));
    /* Each worker runs for a randomized lifespan (12 to 24 beats ~ 24s-48s) before simulating a restart */
    int max_beats = 12 + (rand() % 13);

    int beat = 0;
    while (running) {
        sleep((unsigned)interval);
        if (!running) {
            break;
        }
        beat++;
        if (hb >= 0) {
            char c = 'H';
            if (write(hb, &c, 1) < 0 && errno != EPIPE) {
                perror("heartbeat write");
            }
        }
        printf("[%s] heartbeat %d at %ld\n", name, beat, (long)time(NULL));
        fflush(stdout);

        /* Periodically simulate randomized service fault so the supervisor auto-restarts it */
        if (beat >= max_beats) {
            printf("[%s] simulating randomized service fault & auto-recovery (exit 1) at beat %d\n", name, beat);
            fflush(stdout);
            return 1;
        }
    }

    printf("[%s] worker exiting cleanly\n", name);
    fflush(stdout);
    return 0;
}
