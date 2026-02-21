/*
 * ShrikeOS Monitor — Ring-Buffer Logging Subsystem
 *
 * In-memory circular log buffer with timestamps, level filtering,
 * and query support. Logs can be retrieved from the dashboard.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------ */

#define LOG_BUF_ENTRIES    64
#define LOG_MSG_MAX_LEN    80
#define LOG_MODULE_MAX_LEN 16

/* Log levels */
enum log_level {
	LOG_LVL_DEBUG = 0,
	LOG_LVL_INFO,
	LOG_LVL_WARN,
	LOG_LVL_ERROR,
	LOG_LVL_COUNT,
};

static const char *const log_level_names[] = {
	[LOG_LVL_DEBUG] = "DEBUG",
	[LOG_LVL_INFO]  = "INFO",
	[LOG_LVL_WARN]  = "WARN",
	[LOG_LVL_ERROR] = "ERROR",
};

static const char *const log_level_tags[] = {
	[LOG_LVL_DEBUG] = "[D]",
	[LOG_LVL_INFO]  = "[I]",
	[LOG_LVL_WARN]  = "[W]",
	[LOG_LVL_ERROR] = "[E]",
};

/* Single log entry */
struct log_entry {
	uint32_t       timestamp_ms;
	enum log_level level;
	char           module[LOG_MODULE_MAX_LEN];
	char           message[LOG_MSG_MAX_LEN];
	uint32_t       sequence;
};

/* Circular buffer */
struct log_buffer {
	struct log_entry entries[LOG_BUF_ENTRIES];
	int              head;
	int              count;
	uint32_t         next_seq;
};

/* Statistics */
struct log_stats {
	uint32_t total_messages;
	uint32_t dropped_messages;
	uint32_t per_level[LOG_LVL_COUNT];
	uint32_t queries_performed;
};

/* ------------------------------------------------------------------ */

static struct log_buffer  log_buf;
static struct log_stats   log_st;
static enum log_level     log_min_level = LOG_LVL_DEBUG;

K_MUTEX_DEFINE(log_mutex);

/* --------------------------------------------------------------------
 * Core API
 * ------------------------------------------------------------------ */

/**
 * shrike_log — Write a message to the log buffer.
 *
 * @param level   Severity level.
 * @param module  Module name (e.g. "WDG", "SYS").
 * @param fmt     printf-style format string.
 */
void shrike_log(enum log_level level, const char *module,
		const char *fmt, ...)
{
	if (level < log_min_level) {
		return;
	}

	k_mutex_lock(&log_mutex, K_FOREVER);

	struct log_entry *e = &log_buf.entries[log_buf.head];

	e->timestamp_ms = k_uptime_get_32();
	e->level        = level;
	e->sequence     = log_buf.next_seq++;

	if (module) {
		strncpy(e->module, module, LOG_MODULE_MAX_LEN - 1);
		e->module[LOG_MODULE_MAX_LEN - 1] = '\0';
	} else {
		e->module[0] = '\0';
	}

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->message, LOG_MSG_MAX_LEN, fmt, ap);
	va_end(ap);

	log_buf.head = (log_buf.head + 1) % LOG_BUF_ENTRIES;
	if (log_buf.count < LOG_BUF_ENTRIES) {
		log_buf.count++;
	} else {
		log_st.dropped_messages++;
	}

	log_st.total_messages++;
	if (level < LOG_LVL_COUNT) {
		log_st.per_level[level]++;
	}

	k_mutex_unlock(&log_mutex);
}

/* Convenience macros */
#define SHRIKE_LOG_D(mod, ...) shrike_log(LOG_LVL_DEBUG, mod, __VA_ARGS__)
#define SHRIKE_LOG_I(mod, ...) shrike_log(LOG_LVL_INFO,  mod, __VA_ARGS__)
#define SHRIKE_LOG_W(mod, ...) shrike_log(LOG_LVL_WARN,  mod, __VA_ARGS__)
#define SHRIKE_LOG_E(mod, ...) shrike_log(LOG_LVL_ERROR, mod, __VA_ARGS__)

/**
 * shrike_log_set_level — Set the minimum log level filter.
 */
void shrike_log_set_level(enum log_level min)
{
	k_mutex_lock(&log_mutex, K_FOREVER);
	if (min < LOG_LVL_COUNT) {
		log_min_level = min;
	}
	k_mutex_unlock(&log_mutex);
}

/**
 * shrike_log_get_level — Get the current minimum log level.
 */
enum log_level shrike_log_get_level(void)
{
	enum log_level lvl;
	k_mutex_lock(&log_mutex, K_FOREVER);
	lvl = log_min_level;
	k_mutex_unlock(&log_mutex);
	return lvl;
}

/**
 * shrike_log_clear — Discard all buffered log entries.
 */
void shrike_log_clear(void)
{
	k_mutex_lock(&log_mutex, K_FOREVER);
	log_buf.head  = 0;
	log_buf.count = 0;
	k_mutex_unlock(&log_mutex);
}

/* --------------------------------------------------------------------
 * Query API
 * ------------------------------------------------------------------ */

/**
 * shrike_log_dump — Print all buffered entries to the console.
 *
 * @param min_level  Only show entries at or above this level.
 */
void shrike_log_dump(enum log_level min_level)
{
	k_mutex_lock(&log_mutex, K_FOREVER);

	log_st.queries_performed++;

	int start = (log_buf.head - log_buf.count + LOG_BUF_ENTRIES) %
		    LOG_BUF_ENTRIES;

	printk("\n=== Log Buffer (%d / %d entries, filter >= %s) ===\n",
	       log_buf.count, LOG_BUF_ENTRIES,
	       log_level_names[min_level]);

	int shown = 0;
	for (int i = 0; i < log_buf.count; i++) {
		int idx = (start + i) % LOG_BUF_ENTRIES;
		const struct log_entry *e = &log_buf.entries[idx];

		if (e->level < min_level) {
			continue;
		}

		uint32_t s  = e->timestamp_ms / 1000;
		uint32_t ms = e->timestamp_ms % 1000;

		printk("[%5u.%03u] %s %-8s %s\n",
		       s, ms,
		       log_level_tags[e->level],
		       e->module,
		       e->message);
		shown++;
	}

	printk("=== Shown %d entries ===\n\n", shown);

	k_mutex_unlock(&log_mutex);
}

/**
 * shrike_log_dump_last — Print the N most recent log entries.
 *
 * @param count  Maximum number of entries to show.
 */
void shrike_log_dump_last(int count)
{
	k_mutex_lock(&log_mutex, K_FOREVER);

	log_st.queries_performed++;

	int to_show = (count < log_buf.count) ? count : log_buf.count;
	int start_offset = log_buf.count - to_show;
	int start = (log_buf.head - log_buf.count + LOG_BUF_ENTRIES) %
		    LOG_BUF_ENTRIES;

	printk("\n=== Last %d Log Entries ===\n", to_show);

	for (int i = start_offset; i < log_buf.count; i++) {
		int idx = (start + i) % LOG_BUF_ENTRIES;
		const struct log_entry *e = &log_buf.entries[idx];

		uint32_t s  = e->timestamp_ms / 1000;
		uint32_t ms = e->timestamp_ms % 1000;

		printk("[%5u.%03u] %s %-8s %s\n",
		       s, ms,
		       log_level_tags[e->level],
		       e->module,
		       e->message);
	}

	printk("==========================\n\n");

	k_mutex_unlock(&log_mutex);
}

/**
 * shrike_log_search — Search log entries containing a keyword.
 *
 * @param keyword  Substring to search for (case-sensitive).
 * @param max_results  Maximum matches to print.
 * @return         Number of matches found.
 */
int shrike_log_search(const char *keyword, int max_results)
{
	int found = 0;

	k_mutex_lock(&log_mutex, K_FOREVER);

	log_st.queries_performed++;

	int start = (log_buf.head - log_buf.count + LOG_BUF_ENTRIES) %
		    LOG_BUF_ENTRIES;

	printk("\n=== Log Search: \"%s\" ===\n", keyword);

	for (int i = 0; i < log_buf.count && found < max_results; i++) {
		int idx = (start + i) % LOG_BUF_ENTRIES;
		const struct log_entry *e = &log_buf.entries[idx];

		if (strstr(e->message, keyword) != NULL ||
		    strstr(e->module, keyword) != NULL) {

			uint32_t s  = e->timestamp_ms / 1000;
			uint32_t ms = e->timestamp_ms % 1000;

			printk("[%5u.%03u] %s %-8s %s\n",
			       s, ms,
			       log_level_tags[e->level],
			       e->module,
			       e->message);
			found++;
		}
	}

	printk("=== Found %d matches ===\n\n", found);

	k_mutex_unlock(&log_mutex);
	return found;
}

/**
 * shrike_log_count_by_level — Count entries at a given level.
 */
int shrike_log_count_by_level(enum log_level level)
{
	int count = 0;

	k_mutex_lock(&log_mutex, K_FOREVER);

	int start = (log_buf.head - log_buf.count + LOG_BUF_ENTRIES) %
		    LOG_BUF_ENTRIES;

	for (int i = 0; i < log_buf.count; i++) {
		int idx = (start + i) % LOG_BUF_ENTRIES;
		if (log_buf.entries[idx].level == level) {
			count++;
		}
	}

	k_mutex_unlock(&log_mutex);
	return count;
}

/* --------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------ */

/**
 * shrike_log_dump_stats — Print logging statistics.
 */
void shrike_log_dump_stats(void)
{
	k_mutex_lock(&log_mutex, K_FOREVER);

	printk("\n=== Logging Statistics ===\n");
	printk("Buffer   : %d / %d entries\n",
	       log_buf.count, LOG_BUF_ENTRIES);
	printk("Total    : %u messages\n", log_st.total_messages);
	printk("Dropped  : %u (buffer full)\n", log_st.dropped_messages);
	printk("Queries  : %u\n", log_st.queries_performed);
	printk("Per level:\n");
	for (int i = 0; i < LOG_LVL_COUNT; i++) {
		printk("  %-6s : %u\n",
		       log_level_names[i], log_st.per_level[i]);
	}
	printk("Filter   : >= %s\n", log_level_names[log_min_level]);
	printk("=========================\n\n");

	k_mutex_unlock(&log_mutex);
}

/**
 * shrike_log_format_json — Serialise recent entries as JSON.
 *
 * @param buf      Output buffer.
 * @param buf_len  Size of output buffer.
 * @param count    Maximum number of recent entries to include.
 * @return         Bytes written (excluding NUL).
 */
int shrike_log_format_json(char *buf, size_t buf_len, int count)
{
	int written = 0;

	k_mutex_lock(&log_mutex, K_FOREVER);

	int to_show = (count < log_buf.count) ? count : log_buf.count;
	int start_offset = log_buf.count - to_show;
	int start = (log_buf.head - log_buf.count + LOG_BUF_ENTRIES) %
		    LOG_BUF_ENTRIES;

	written += snprintf(buf + written, buf_len - written,
			    "{\"log_count\":%d,\"total\":%u,"
			    "\"dropped\":%u,\"entries\":[",
			    log_buf.count,
			    log_st.total_messages,
			    log_st.dropped_messages);

	for (int i = start_offset; i < log_buf.count; i++) {
		int idx = (start + i) % LOG_BUF_ENTRIES;
		const struct log_entry *e = &log_buf.entries[idx];

		if (written >= (int)buf_len - 2) break;

		if (i > start_offset) {
			written += snprintf(buf + written,
					    buf_len - written, ",");
		}

		written += snprintf(buf + written, buf_len - written,
			"{\"t\":%u,\"l\":\"%s\",\"m\":\"%s\","
			"\"msg\":\"%s\",\"seq\":%u}",
			e->timestamp_ms,
			log_level_names[e->level],
			e->module,
			e->message,
			e->sequence);
	}

	written += snprintf(buf + written, buf_len - written, "]}");

	k_mutex_unlock(&log_mutex);
	return written;
}

/* ------------------------------------------------------------------ */

/**
 * shrike_log_init — Initialise the logging subsystem.
 */
void shrike_log_init(void)
{
	memset(&log_buf, 0, sizeof(log_buf));
	memset(&log_st, 0, sizeof(log_st));
	log_min_level = LOG_LVL_DEBUG;

	SHRIKE_LOG_I("LOG", "Logging subsystem initialised "
		     "(%d entry buffer)", LOG_BUF_ENTRIES);

	printk("[LOG] Ring-buffer logger ready "
	       "(%d slots, filter >= %s)\n",
	       LOG_BUF_ENTRIES, log_level_names[log_min_level]);
}
