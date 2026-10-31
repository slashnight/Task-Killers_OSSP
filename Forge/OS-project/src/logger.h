#ifndef LOGGER_H
#define LOGGER_H

#include "supervisor.h"

int logger_init(const char *path);
void logger_close(void);
void log_event(const char *level, const char *fmt, ...);
void log_rusage(const char *service_name);

#endif
