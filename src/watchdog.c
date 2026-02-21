/*
 * ShrikeOS Monitor — Software Watchdog Manager
 *
 * Monitors thread health by tracking periodic heartbeats from each
 * registered thread. If a thread fails to report within its configured
 * timeout, the watchdog flags it as unresponsive and invokes the
 * registered recovery callback.
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

#define WDG_MAX_THREADS        8
#define WDG_CHECK_INTERVAL_MS  1000
#define WDG_STACK_SIZE         1024
#define WDG_PRIORITY           8

/* Thread health states reported by the watchdog */
enum wdg_thread_state {
	WDG_STATE_IDLE = 0,    /* Registered, not yet started        */
	WDG_STATE_HEALTHY,     /* Heartbeat received within timeout   */
	WDG_STATE_WARNING,     /* Approaching timeout (>75% elapsed)  */
	WDG_STATE_UNRESPONSIVE,/* Timed out — recovery pending        */
	WDG_STATE_RECOVERED,   /* Recovery callback executed           */
};

/* Human-readable names for each state (used by status dump) */
static const char *const wdg_state_names[] = {
	[WDG_STATE_IDLE]         = "IDLE",
	[WDG_STATE_HEALTHY]      = "HEALTHY",
	[WDG_STATE_WARNING]      = "WARNING",
	[WDG_STATE_UNRESPONSIVE] = "UNRESPONSIVE",
	[WDG_STATE_RECOVERED]    = "RECOVERED",
};

/* Callback invoked when a thread becomes unresponsive.
 * The callback receives the thread name and the elapsed time in ms
 * since the last heartbeat was received.
 */
typedef void (*wdg_recovery_cb_t)(const char *thread_name,
				  uint32_t elapsed_ms);

/* Internal bookkeeping for a single monitored thread */
struct wdg_entry {
	bool          active;
	char          name[24];
	uint32_t      timeout_ms;
	int64_t       last_heartbeat;
	enum wdg_thread_state state;
	wdg_recovery_cb_t     recovery_cb;
	uint32_t      heartbeat_count;
	uint32_t      timeout_count;
	uint32_t      recovery_count;
};

/* Registry of all monitored threads */
static struct wdg_entry wdg_table[WDG_MAX_THREADS];
static int              wdg_count;

/* Mutex protecting the watchdog table */
K_MUTEX_DEFINE(wdg_mutex);

/* Global enable flag — allows the watchdog to be suspended */
static bool wdg_enabled = true;

/* Aggregate statistics */
static struct wdg_stats {
	uint32_t total_heartbeats;
	uint32_t total_timeouts;
	uint32_t total_recoveries;
	uint32_t checks_performed;
} wdg_stats;

/* --------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/**
 * wdg_register — Register a thread for watchdog monitoring.
 *
 * @param name        Human-readable thread name (max 23 chars).
 * @param timeout_ms  Maximum allowed time between heartbeats.
 * @param cb          Optional recovery callback (may be NULL).
 * @return            Slot index (≥ 0) on success, -1 if table full.
 */
int wdg_register(const char *name, uint32_t timeout_ms,
		 wdg_recovery_cb_t cb)
{
	int slot = -1;

	k_mutex_lock(&wdg_mutex, K_FOREVER);

	if (wdg_count >= WDG_MAX_THREADS) {
		printk("[WDG] Table full, cannot register '%s'\n", name);
		k_mutex_unlock(&wdg_mutex);
		return -1;
	}

	slot = wdg_count++;
	struct wdg_entry *e = &wdg_table[slot];

	memset(e, 0, sizeof(*e));
	e->active = true;
	strncpy(e->name, name, sizeof(e->name) - 1);
	e->name[sizeof(e->name) - 1] = '\0';
	e->timeout_ms      = timeout_ms;
	e->last_heartbeat   = k_uptime_get();
	e->state            = WDG_STATE_IDLE;
	e->recovery_cb      = cb;
	e->heartbeat_count  = 0;
	e->timeout_count    = 0;
	e->recovery_count   = 0;

	printk("[WDG] Registered '%s' (slot %d, timeout %u ms)\n",
	       name, slot, timeout_ms);

	k_mutex_unlock(&wdg_mutex);
	return slot;
}

/**
 * wdg_heartbeat — Signal that a thread is still alive.
 *
 * @param slot  Slot index returned by wdg_register().
 */
void wdg_heartbeat(int slot)
{
	if (slot < 0 || slot >= WDG_MAX_THREADS) {
		return;
	}

	k_mutex_lock(&wdg_mutex, K_FOREVER);

	struct wdg_entry *e = &wdg_table[slot];
	if (e->active) {
		e->last_heartbeat  = k_uptime_get();
		e->state           = WDG_STATE_HEALTHY;
		e->heartbeat_count++;
		wdg_stats.total_heartbeats++;
	}

	k_mutex_unlock(&wdg_mutex);
}

/**
 * wdg_unregister — Remove a thread from monitoring.
 *
 * @param slot  Slot index returned by wdg_register().
 */
void wdg_unregister(int slot)
{
	if (slot < 0 || slot >= WDG_MAX_THREADS) {
		return;
	}

	k_mutex_lock(&wdg_mutex, K_FOREVER);

	struct wdg_entry *e = &wdg_table[slot];
	if (e->active) {
		printk("[WDG] Unregistered '%s' (slot %d)\n", e->name, slot);
		e->active = false;
	}

	k_mutex_unlock(&wdg_mutex);
}

/**
 * wdg_enable — Enable or disable the watchdog globally.
 */
void wdg_enable(bool enable)
{
	k_mutex_lock(&wdg_mutex, K_FOREVER);
	wdg_enabled = enable;
	printk("[WDG] Watchdog %s\n", enable ? "enabled" : "disabled");
	k_mutex_unlock(&wdg_mutex);
}

/**
 * wdg_get_state — Query the current state of a monitored thread.
 *
 * @param slot  Slot index returned by wdg_register().
 * @return      Current wdg_thread_state, or WDG_STATE_IDLE on error.
 */
enum wdg_thread_state wdg_get_state(int slot)
{
	if (slot < 0 || slot >= WDG_MAX_THREADS) {
		return WDG_STATE_IDLE;
	}

	enum wdg_thread_state st;

	k_mutex_lock(&wdg_mutex, K_FOREVER);
	st = wdg_table[slot].state;
	k_mutex_unlock(&wdg_mutex);

	return st;
}

/**
 * wdg_get_state_name — Return a human-readable string for a state.
 */
const char *wdg_get_state_name(enum wdg_thread_state st)
{
	if (st >= ARRAY_SIZE(wdg_state_names)) {
		return "UNKNOWN";
	}
	return wdg_state_names[st];
}

/**
 * wdg_get_healthy_count — Return how many threads are currently healthy.
 */
int wdg_get_healthy_count(void)
{
	int count = 0;

	k_mutex_lock(&wdg_mutex, K_FOREVER);
	for (int i = 0; i < wdg_count; i++) {
		if (wdg_table[i].active &&
		    wdg_table[i].state == WDG_STATE_HEALTHY) {
			count++;
		}
	}
	k_mutex_unlock(&wdg_mutex);

	return count;
}

/**
 * wdg_dump_status — Print the full watchdog status table to the console.
 */
void wdg_dump_status(void)
{
	k_mutex_lock(&wdg_mutex, K_FOREVER);

	printk("\n=== Watchdog Status ===\n");
	printk("Global: %s | Checks: %u | Heartbeats: %u | "
	       "Timeouts: %u | Recoveries: %u\n",
	       wdg_enabled ? "ENABLED" : "DISABLED",
	       wdg_stats.checks_performed,
	       wdg_stats.total_heartbeats,
	       wdg_stats.total_timeouts,
	       wdg_stats.total_recoveries);
	printk("%-4s %-20s %-14s %-10s %-6s %-6s\n",
	       "Slot", "Name", "State", "Timeout",
	       "Beats", "Fails");
	printk("---- -------------------- -------------- "
	       "---------- ------ ------\n");

	for (int i = 0; i < wdg_count; i++) {
		const struct wdg_entry *e = &wdg_table[i];
		if (!e->active) {
			continue;
		}
		printk("%-4d %-20s %-14s %-10u %-6u %-6u\n",
		       i, e->name,
		       wdg_get_state_name(e->state),
		       e->timeout_ms,
		       e->heartbeat_count,
		       e->timeout_count);
	}

	printk("=======================\n\n");

	k_mutex_unlock(&wdg_mutex);
}

/* Default recovery handler used when no callback is provided */
static void wdg_default_recovery(const char *name, uint32_t elapsed_ms)
{
	printk("[WDG] DEFAULT RECOVERY for '%s' "
	       "(no heartbeat for %u ms)\n", name, elapsed_ms);
}

/* --------------------------------------------------------------------
 * Watchdog checker thread
 * ------------------------------------------------------------------ */

/**
 * The checker thread runs periodically to evaluate the health of every
 * registered thread.  For each thread it:
 *   1. Computes elapsed time since the last heartbeat.
 *   2. Transitions to WARNING if >75% of the timeout has elapsed.
 *   3. Transitions to UNRESPONSIVE if the full timeout has elapsed.
 *   4. Invokes the recovery callback on the first UNRESPONSIVE event.
 */
static void wdg_checker_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	printk("[WDG] Watchdog checker thread started "
	       "(interval %d ms)\n", WDG_CHECK_INTERVAL_MS);

	while (1) {
		k_msleep(WDG_CHECK_INTERVAL_MS);

		k_mutex_lock(&wdg_mutex, K_FOREVER);

		if (!wdg_enabled) {
			k_mutex_unlock(&wdg_mutex);
			continue;
		}

		wdg_stats.checks_performed++;
		int64_t now = k_uptime_get();

		for (int i = 0; i < wdg_count; i++) {
			struct wdg_entry *e = &wdg_table[i];
			if (!e->active) {
				continue;
			}

			int64_t elapsed = now - e->last_heartbeat;

			if (elapsed > (int64_t)e->timeout_ms) {
				/* Full timeout reached */
				if (e->state != WDG_STATE_UNRESPONSIVE &&
				    e->state != WDG_STATE_RECOVERED) {
					e->state = WDG_STATE_UNRESPONSIVE;
					e->timeout_count++;
					wdg_stats.total_timeouts++;

					printk("[WDG] '%s' UNRESPONSIVE "
					       "(%lld ms elapsed)\n",
					       e->name, elapsed);

					wdg_recovery_cb_t cb = e->recovery_cb;
					const char *ename = e->name;
					uint32_t el = (uint32_t)elapsed;

					/* Release mutex before callback */
					k_mutex_unlock(&wdg_mutex);

					if (cb) {
						cb(ename, el);
					} else {
						wdg_default_recovery(ename,
								     el);
					}

					k_mutex_lock(&wdg_mutex, K_FOREVER);

					e->state = WDG_STATE_RECOVERED;
					e->recovery_count++;
					wdg_stats.total_recoveries++;
				}
			} else if (elapsed >
				   (int64_t)(e->timeout_ms * 3 / 4)) {
				/* 75% threshold → warning zone */
				if (e->state == WDG_STATE_HEALTHY) {
					e->state = WDG_STATE_WARNING;
					printk("[WDG] '%s' entering "
					       "WARNING zone\n", e->name);
				}
			}
		}

		k_mutex_unlock(&wdg_mutex);
	}
}

K_THREAD_DEFINE(wdg_checker_tid, WDG_STACK_SIZE,
		wdg_checker_fn, NULL, NULL, NULL,
		WDG_PRIORITY, 0, 0);
