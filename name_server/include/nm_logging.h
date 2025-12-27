#ifndef NM_LOGGING_H
#define NM_LOGGING_H

#include "nm_common.h"

void log_message(const char *level, const char *message, const char *ip, int port, const char *username);

#endif /* NM_LOGGING_H */
