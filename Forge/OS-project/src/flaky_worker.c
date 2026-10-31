#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

static volatile sig_atomic_t running = 1;

static void on_term(int signo) {
    (void)signo;
    running = 0;
}

int main(void) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int hb = -1;
    const char *fdstr = getenv("SUPERVISOR_HB_FD");
    if (fdstr) {
        hb = atoi(fdstr);
    }

    printf("[flaky] worker started pid=%d heartbeat_fd=%d\n", (int)getpid(), hb);
    fflush(stdout);

    /* Seed PRNG uniquely per process instance */
    srand((unsigned)(time(NULL) ^ ((unsigned)getpid() << 16)));
    int max_beats = 10 + (rand() % 10); /* ~20s to 38s */

    int beat = 0;
    while (running) {
        sleep(2);
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
        printf("[flaky] heartbeat %d at %ld\n", beat, (long)time(NULL));
        fflush(stdout);

        if (beat >= max_beats) {
            printf("[flaky] simulating randomized service fault & auto-recovery (exit 1) at beat %d\n", beat);
            fflush(stdout);
            return 1;
        }
    }

    printf("[flaky] worker exiting cleanly\n");
    fflush(stdout);
    return 0;
}
