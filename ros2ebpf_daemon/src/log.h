/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ROS2EBPF_LOG_H
#define ROS2EBPF_LOG_H
#include <stdio.h>
#include <time.h>

enum log_level { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };
extern int g_log_level;

#define LOGF(lvl, tag, fmt, ...) do {                                             \
        if ((lvl) <= g_log_level) {                                               \
            struct timespec _ts; clock_gettime(CLOCK_REALTIME, &_ts);             \
            struct tm _tm; gmtime_r(&_ts.tv_sec, &_tm);                           \
            fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02dZ %s " fmt "\n",        \
                    _tm.tm_year + 1900, _tm.tm_mon + 1, _tm.tm_mday,              \
                    _tm.tm_hour, _tm.tm_min, _tm.tm_sec, tag, ##__VA_ARGS__);     \
        }                                                                         \
    } while (0)

#define log_error(fmt, ...) LOGF(LOG_ERROR, "E", fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  LOGF(LOG_WARN,  "W", fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  LOGF(LOG_INFO,  "I", fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) LOGF(LOG_DEBUG, "D", fmt, ##__VA_ARGS__)

#endif
