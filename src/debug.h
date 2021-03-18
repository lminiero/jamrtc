/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

#ifndef JAMRTC_DEBUG_H
#define JAMRTC_DEBUG_H

#include <inttypes.h>

#include <glib.h>
#include <glib/gprintf.h>

extern int jamrtc_log_level;
extern gboolean jamrtc_log_timestamps;
extern gboolean jamrtc_log_colors;

/* Log colors */
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/* Log levels */
#define LOG_NONE     (0)
#define LOG_FATAL    (1)
#define LOG_ERR      (2)
#define LOG_WARN     (3)
#define LOG_INFO     (4)
#define LOG_VERB     (5)
#define LOG_HUGE     (6)
#define LOG_DBG      (7)
#define LOG_MAX LOG_DBG

/* Coloured prefixes for errors and warnings logging. */
static const char *jamrtc_log_prefix[] = {
/* no colors */
	"",
	"[FATAL] ",
	"[ERR] ",
	"[WARN] ",
	"",
	"",
	"",
	"",
/* with colors */
	"",
	ANSI_COLOR_MAGENTA"[FATAL]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_RED"[ERR]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_YELLOW"[WARN]"ANSI_COLOR_RESET" ",
	"",
	"",
	"",
	""
};

/* Simple wrapper to g_print/printf */
#define JAMRTC_PRINT g_print
/* Logger based on different levels, which can either be displayed
 * or not according to the configuration of the gateway.
 * The format must be a string literal. */
#define JAMRTC_LOG(level, format, ...) \
do { \
	if (level > LOG_NONE && level <= LOG_MAX && level <= jamrtc_log_level) { \
		char jamrtc_log_ts[64] = ""; \
		char jamrtc_log_src[128] = ""; \
		if (jamrtc_log_timestamps) { \
			struct tm jamrtctmresult; \
			time_t jamrtcltime = time(NULL); \
			localtime_r(&jamrtcltime, &jamrtctmresult); \
			strftime(jamrtc_log_ts, sizeof(jamrtc_log_ts), \
			         "[%a %b %e %T %Y] ", &jamrtctmresult); \
		} \
		if (level == LOG_FATAL || level == LOG_ERR || level == LOG_DBG) { \
			snprintf(jamrtc_log_src, sizeof(jamrtc_log_src), \
			         "[%s:%s:%d] ", __FILE__, __FUNCTION__, __LINE__); \
		} \
		g_print("%s%s%s" format, \
		        jamrtc_log_ts, \
		        jamrtc_log_prefix[level | ((int)jamrtc_log_colors << 3)], \
		        jamrtc_log_src, \
		        ##__VA_ARGS__); \
	} \
} while (0)

#endif
