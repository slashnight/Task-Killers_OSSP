CC ?= gcc
CFLAGS := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Iinclude
LDFLAGS :=

BIN_DIR := bin
LOG_DIR := logs

SUPERVISOR := $(BIN_DIR)/supervisor
DUMMY := $(BIN_DIR)/dummy_worker
FLAKY := $(BIN_DIR)/flaky_worker
HUNG := $(BIN_DIR)/hung_worker

SUPERVISOR_SRC := src/supervisor.c src/config.c src/process.c src/logger.c src/signals.c src/http_server.c

.PHONY: all clean dirs run test

all: dirs $(SUPERVISOR) $(DUMMY) $(FLAKY) $(HUNG)

dirs:
	mkdir -p $(BIN_DIR) $(LOG_DIR)

$(SUPERVISOR): $(SUPERVISOR_SRC) include/supervisor.h
	$(CC) $(CFLAGS) -o $@ $(SUPERVISOR_SRC) $(LDFLAGS)

$(DUMMY): src/dummy_worker.c
	$(CC) $(CFLAGS) -o $@ $<

$(FLAKY): src/flaky_worker.c
	$(CC) $(CFLAGS) -o $@ $<

$(HUNG): src/hung_worker.c
	$(CC) $(CFLAGS) -o $@ $<

run: all
	./$(SUPERVISOR) -c config/services.conf

test: all
	printf 'status\nquit\n' | ./$(SUPERVISOR) -c config/services.conf --no-start

clean:
	rm -f $(SUPERVISOR) $(DUMMY) $(FLAKY) $(HUNG)
