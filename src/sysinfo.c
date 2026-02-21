/*
 * ShrikeOS Monitor — System Information & Diagnostics Module
 *
 * Gathers comprehensive system-level metrics for the ShrikeOS monitor
 * dashboard including memory statistics, thread enumeration, CPU load
 * estimation, and build/version information.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------ */

#define SYSINFO_MAX_THREADS       16
#define SYSINFO_UPDATE_INTERVAL   2000   /* ms between metric refreshes   */
#define SYSINFO_STACK_SIZE        1536
#define SYSINFO_PRIORITY          9

/* Build metadata embedded at compile time */
#define SHRIKE_FW_VERSION_MAJOR   1
#define SHRIKE_FW_VERSION_MINOR   2
#define SHRIKE_FW_VERSION_PATCH   0
#define SHRIKE_BOARD_NAME         "Shrike-lite (RP2040 + SLG47910)"

/* --------------------------------------------------------------------
 * Data Structures
 * ------------------------------------------------------------------ */

/* Per-thread diagnostic snapshot */
struct sysinfo_thread {
	char     name[20];
	uint32_t stack_size;
	uint32_t stack_used;
	uint8_t  priority;
	uint8_t  state;          /* 0 = ready, 1 = running, 2 = waiting */
	bool     valid;
};

/* Aggregate system metrics */
struct sysinfo_snapshot {
	/* Timing */
	uint32_t uptime_secs;
	uint32_t uptime_ms;

	/* Memory */
	uint32_t heap_total;
	uint32_t heap_used;
	uint32_t heap_free;
	uint32_t heap_max_used;

	/* Threads */
	uint8_t  thread_count;
	struct sysinfo_thread threads[SYSINFO_MAX_THREADS];

	/* CPU estimate (simple busy/idle ratio) */
	uint8_t  cpu_load_pct;

	/* Boot counter (persisted in RAM across soft resets if supported) */
	uint32_t boot_count;

	/* Firmware version */
	uint8_t  fw_major;
	uint8_t  fw_minor;
	uint8_t  fw_patch;
};

/* The latest snapshot (protected by mutex) */
static struct sysinfo_snapshot snapshot;
K_MUTEX_DEFINE(sysinfo_mutex);

/* Idle-time tracking for CPU load estimate */
static volatile uint64_t idle_ticks_prev;
static volatile uint64_t total_ticks_prev;
static uint32_t boot_counter;

/* --------------------------------------------------------------------
 * Internal Helpers
 * ------------------------------------------------------------------ */

/**
 * Estimate heap utilisation.
 *
 * Zephyr's k_heap_* APIs are only available when a system heap is
 * configured.  On builds without CONFIG_HEAP_MEM_POOL_SIZE we fall
 * back to compiled-in constants so the rest of the code compiles
 * unconditionally.
 */
static void sysinfo_update_heap(struct sysinfo_snapshot *s)
{
#ifdef CONFIG_HEAP_MEM_POOL_SIZE
	struct sys_memory_stats stats;
	sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
	s->heap_total    = (uint32_t)stats.free_bytes +
			   (uint32_t)stats.allocated_bytes;
	s->heap_used     = (uint32_t)stats.allocated_bytes;
	s->heap_free     = (uint32_t)stats.free_bytes;
	s->heap_max_used = (uint32_t)stats.max_allocated_bytes;
#else
	s->heap_total    = 0;
	s->heap_used     = 0;
	s->heap_free     = 0;
	s->heap_max_used = 0;
#endif
}

/**
 * Walk the Zephyr thread list and populate per-thread info.
 *
 * We use k_thread_foreach_unlocked() which does NOT hold the scheduler
 * lock — fine for a diagnostic snapshot.
 */
struct thread_walk_ctx {
	struct sysinfo_snapshot *snap;
	int                     idx;
};

static void thread_info_cb(const struct k_thread *thread, void *user_data)
{
	struct thread_walk_ctx *ctx = user_data;

	if (ctx->idx >= SYSINFO_MAX_THREADS) {
		return;
	}

	struct sysinfo_thread *t = &ctx->snap->threads[ctx->idx];
	t->valid = true;

	/* Thread name (may be NULL if not configured) */
	const char *tname = k_thread_name_get((k_tid_t)thread);
	if (tname && tname[0] != '\0') {
		strncpy(t->name, tname, sizeof(t->name) - 1);
		t->name[sizeof(t->name) - 1] = '\0';
	} else {
		snprintf(t->name, sizeof(t->name), "thread_%d", ctx->idx);
	}

	/* Stack analysis */
#ifdef CONFIG_THREAD_STACK_INFO
	t->stack_size = thread->stack_info.size;
#else
	t->stack_size = 0;
#endif

#ifdef CONFIG_INIT_STACKS
	size_t unused = 0;
	if (k_thread_stack_space_get(thread, &unused) == 0) {
		t->stack_used = t->stack_size - (uint32_t)unused;
	} else {
		t->stack_used = 0;
	}
#else
	t->stack_used = 0;
#endif

	t->priority = (uint8_t)k_thread_priority_get((k_tid_t)thread);

	/* Coarse state mapping */
	if (k_is_in_isr()) {
		t->state = 1;  /* approximate */
	} else {
		t->state = 0;
	}

	ctx->idx++;
}

static void sysinfo_update_threads(struct sysinfo_snapshot *s)
{
	/* Reset thread slots */
	for (int i = 0; i < SYSINFO_MAX_THREADS; i++) {
		s->threads[i].valid = false;
	}

	struct thread_walk_ctx ctx = { .snap = s, .idx = 0 };
	k_thread_foreach_unlocked(thread_info_cb, &ctx);
	s->thread_count = (uint8_t)ctx.idx;
}

/**
 * Very rough CPU-load estimator.
 *
 * Uses the Zephyr idle thread's cycle count (if runtime stats are
 * enabled) to compute an approximate load percentage.  When the stats
 * are unavailable we report 0.
 */
static void sysinfo_update_cpu(struct sysinfo_snapshot *s)
{
#ifdef CONFIG_THREAD_RUNTIME_STATS
	k_thread_runtime_stats_t rt_stats;
	int ret = k_thread_runtime_stats_all_get(&rt_stats);
	if (ret == 0 && rt_stats.total_cycles > 0) {
		uint64_t total = rt_stats.total_cycles;

		/* idle cycles are part of total; approximate */
		uint64_t busy = total;  /* placeholder */
		k_thread_runtime_stats_t idle_stats;
		k_tid_t idle = k_thread_find(NULL);  /* heuristic */
		if (idle && k_thread_runtime_stats_get(idle,
						       &idle_stats) == 0) {
			busy = total - idle_stats.total_cycles;
		}

		if (total > total_ticks_prev) {
			uint64_t dt = total - total_ticks_prev;
			uint64_t db = busy - idle_ticks_prev;
			s->cpu_load_pct = (uint8_t)((db * 100) / dt);
		}

		total_ticks_prev = total;
		idle_ticks_prev  = busy;
	} else {
		s->cpu_load_pct = 0;
	}
#else
	s->cpu_load_pct = 0;
#endif
}

/* --------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/**
 * sysinfo_get — Copy the latest snapshot into the caller's buffer.
 *
 * @param out  Destination buffer (must not be NULL).
 */
void sysinfo_get(struct sysinfo_snapshot *out)
{
	k_mutex_lock(&sysinfo_mutex, K_FOREVER);
	memcpy(out, &snapshot, sizeof(snapshot));
	k_mutex_unlock(&sysinfo_mutex);
}

/**
 * sysinfo_get_uptime_secs — Quick accessor for uptime.
 */
uint32_t sysinfo_get_uptime_secs(void)
{
	uint32_t up;

	k_mutex_lock(&sysinfo_mutex, K_FOREVER);
	up = snapshot.uptime_secs;
	k_mutex_unlock(&sysinfo_mutex);

	return up;
}

/**
 * sysinfo_get_thread_count — Return the number of active threads.
 */
uint8_t sysinfo_get_thread_count(void)
{
	uint8_t n;

	k_mutex_lock(&sysinfo_mutex, K_FOREVER);
	n = snapshot.thread_count;
	k_mutex_unlock(&sysinfo_mutex);

	return n;
}

/**
 * sysinfo_get_cpu_load — Return estimated CPU usage (0–100).
 */
uint8_t sysinfo_get_cpu_load(void)
{
	uint8_t load;

	k_mutex_lock(&sysinfo_mutex, K_FOREVER);
	load = snapshot.cpu_load_pct;
	k_mutex_unlock(&sysinfo_mutex);

	return load;
}

/**
 * sysinfo_get_fw_version — Write the firmware version string into buf.
 *
 * @param buf      Output buffer.
 * @param buf_len  Size of buf in bytes.
 * @return         Number of characters written (excluding NUL).
 */
int sysinfo_get_fw_version(char *buf, size_t buf_len)
{
	return snprintf(buf, buf_len, "%u.%u.%u",
			SHRIKE_FW_VERSION_MAJOR,
			SHRIKE_FW_VERSION_MINOR,
			SHRIKE_FW_VERSION_PATCH);
}

/**
 * sysinfo_get_board_name — Return a pointer to the static board name.
 */
const char *sysinfo_get_board_name(void)
{
	return SHRIKE_BOARD_NAME;
}

/**
 * sysinfo_dump — Print a comprehensive system report to the console.
 */
void sysinfo_dump(void)
{
	k_mutex_lock(&sysinfo_mutex, K_FOREVER);

	printk("\n=== ShrikeOS System Info ===\n");
	printk("Board     : %s\n", SHRIKE_BOARD_NAME);
	printk("Firmware  : %u.%u.%u\n",
	       SHRIKE_FW_VERSION_MAJOR,
	       SHRIKE_FW_VERSION_MINOR,
	       SHRIKE_FW_VERSION_PATCH);
	printk("Zephyr    : %s\n", KERNEL_VERSION_STRING);
	printk("Uptime    : %u s (%u ms)\n",
	       snapshot.uptime_secs, snapshot.uptime_ms);
	printk("Boot #    : %u\n", snapshot.boot_count);
	printk("CPU load  : ~%u%%\n", snapshot.cpu_load_pct);
	printk("Heap total: %u B | used: %u B | free: %u B | peak: %u B\n",
	       snapshot.heap_total, snapshot.heap_used,
	       snapshot.heap_free, snapshot.heap_max_used);

	printk("Threads   : %u\n", snapshot.thread_count);
	printk("%-4s %-18s %-6s %-10s %-10s\n",
	       "#", "Name", "Prio", "Stack", "Used");
	printk("---- ------------------ ------ ---------- ----------\n");

	for (int i = 0; i < snapshot.thread_count; i++) {
		const struct sysinfo_thread *t = &snapshot.threads[i];
		if (!t->valid) {
			continue;
		}
		printk("%-4d %-18s %-6u %-10u %-10u\n",
		       i, t->name, t->priority,
		       t->stack_size, t->stack_used);
	}

	printk("===========================\n\n");

	k_mutex_unlock(&sysinfo_mutex);
}

/**
 * sysinfo_format_json — Serialise the current snapshot as a JSON string.
 *
 * @param buf      Destination buffer.
 * @param buf_len  Size of the destination buffer.
 * @return         Number of characters written (excluding NUL), or
 *                 negative on error.
 */
int sysinfo_format_json(char *buf, size_t buf_len)
{
	int written;

	k_mutex_lock(&sysinfo_mutex, K_FOREVER);

	written = snprintf(buf, buf_len,
		"{\"board\":\"%s\","
		"\"fw\":\"%u.%u.%u\","
		"\"up\":%u,"
		"\"cpu\":%u,"
		"\"heap_total\":%u,"
		"\"heap_used\":%u,"
		"\"heap_free\":%u,"
		"\"threads\":%u,"
		"\"boots\":%u}",
		SHRIKE_BOARD_NAME,
		SHRIKE_FW_VERSION_MAJOR,
		SHRIKE_FW_VERSION_MINOR,
		SHRIKE_FW_VERSION_PATCH,
		snapshot.uptime_secs,
		snapshot.cpu_load_pct,
		snapshot.heap_total,
		snapshot.heap_used,
		snapshot.heap_free,
		snapshot.thread_count,
		snapshot.boot_count);

	k_mutex_unlock(&sysinfo_mutex);
	return written;
}

/* --------------------------------------------------------------------
 * Background refresh thread
 * ------------------------------------------------------------------ */

static void sysinfo_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	boot_counter++;

	printk("[SYSINFO] Diagnostics thread started "
	       "(interval %d ms, boot #%u)\n",
	       SYSINFO_UPDATE_INTERVAL, boot_counter);

	while (1) {
		k_mutex_lock(&sysinfo_mutex, K_FOREVER);

		snapshot.uptime_ms   = k_uptime_get_32();
		snapshot.uptime_secs = snapshot.uptime_ms / 1000;
		snapshot.boot_count  = boot_counter;
		snapshot.fw_major    = SHRIKE_FW_VERSION_MAJOR;
		snapshot.fw_minor    = SHRIKE_FW_VERSION_MINOR;
		snapshot.fw_patch    = SHRIKE_FW_VERSION_PATCH;

		sysinfo_update_heap(&snapshot);
		sysinfo_update_threads(&snapshot);
		sysinfo_update_cpu(&snapshot);

		k_mutex_unlock(&sysinfo_mutex);

		k_msleep(SYSINFO_UPDATE_INTERVAL);
	}
}

K_THREAD_DEFINE(sysinfo_tid, SYSINFO_STACK_SIZE,
		sysinfo_thread_fn, NULL, NULL, NULL,
		SYSINFO_PRIORITY, 0, 0);
