/*
 * ShrikeOS Monitor — Command Processing Engine
 *
 * Table-driven command parser for any transport (USB-CDC, UART, BLE).
 * Commands are registered at compile time and dispatched by name with
 * argument parsing, validation, and help output.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define CMD_MAX_COMMANDS   24
#define CMD_MAX_ARGS       8
#define CMD_MAX_LINE       128
#define CMD_HISTORY_DEPTH  8

enum cmd_arg_type {
	CMD_ARG_NONE = 0,
	CMD_ARG_INT,
	CMD_ARG_STRING,
	CMD_ARG_BOOL,
};

struct cmd_arg {
	enum cmd_arg_type type;
	union {
		int         ival;
		const char *sval;
		bool        bval;
	};
};

typedef int (*cmd_handler_t)(int argc, struct cmd_arg *argv);

struct cmd_entry {
	const char    *name;
	const char    *help;
	const char    *usage;
	cmd_handler_t  handler;
	uint8_t        min_args;
	uint8_t        max_args;
	bool           hidden;
};

struct cmd_history {
	char lines[CMD_HISTORY_DEPTH][CMD_MAX_LINE];
	int  head;
	int  count;
};

static struct cmd_entry   cmd_table[CMD_MAX_COMMANDS];
static int                cmd_count;
static struct cmd_history cmd_hist;

static struct cmd_stats {
	uint32_t total_commands;
	uint32_t successful;
	uint32_t failed;
	uint32_t unknown;
	uint32_t arg_errors;
} cmd_stats;

K_MUTEX_DEFINE(cmd_mutex);

typedef void (*cmd_output_fn_t)(const char *str);
static cmd_output_fn_t cmd_output = NULL;

static void cmd_print(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (cmd_output) {
		cmd_output(buf);
	} else {
		printk("%s", buf);
	}
}

/* ---- History ---- */

static void history_add(const char *line)
{
	if (line[0] == '\0') return;

	if (cmd_hist.count > 0) {
		int prev = (cmd_hist.head - 1 + CMD_HISTORY_DEPTH) %
			   CMD_HISTORY_DEPTH;
		if (strcmp(cmd_hist.lines[prev], line) == 0) return;
	}

	strncpy(cmd_hist.lines[cmd_hist.head], line, CMD_MAX_LINE - 1);
	cmd_hist.lines[cmd_hist.head][CMD_MAX_LINE - 1] = '\0';
	cmd_hist.head = (cmd_hist.head + 1) % CMD_HISTORY_DEPTH;
	if (cmd_hist.count < CMD_HISTORY_DEPTH) cmd_hist.count++;
}

void cmd_history_dump(void)
{
	cmd_print("Command history (%d entries):\n", cmd_hist.count);
	int start = (cmd_hist.head - cmd_hist.count + CMD_HISTORY_DEPTH) %
		    CMD_HISTORY_DEPTH;
	for (int i = 0; i < cmd_hist.count; i++) {
		int idx = (start + i) % CMD_HISTORY_DEPTH;
		cmd_print("  [%d] %s\n", i + 1, cmd_hist.lines[idx]);
	}
}

/* ---- Tokeniser ---- */

static int tokenise(char *line, char **tokens, int max_tok)
{
	int count = 0;
	char *p = line;

	while (*p && count < max_tok) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (*p == '\0') break;

		if (*p == '"') {
			p++;
			tokens[count++] = p;
			char *end = strchr(p, '"');
			if (end) { *end = '\0'; p = end + 1; }
			else break;
		} else {
			tokens[count++] = p;
			while (*p && !isspace((unsigned char)*p)) p++;
			if (*p) *p++ = '\0';
		}
	}
	return count;
}

static struct cmd_arg parse_auto(const char *s)
{
	struct cmd_arg arg;

	if (strcmp(s, "true") == 0 || strcmp(s, "on") == 0 ||
	    strcmp(s, "yes") == 0) {
		arg.type = CMD_ARG_BOOL; arg.bval = true; return arg;
	}
	if (strcmp(s, "false") == 0 || strcmp(s, "off") == 0 ||
	    strcmp(s, "no") == 0) {
		arg.type = CMD_ARG_BOOL; arg.bval = false; return arg;
	}

	char *end;
	long lval = strtol(s, &end, 0);
	if (*end == '\0' && end != s) {
		arg.type = CMD_ARG_INT; arg.ival = (int)lval; return arg;
	}

	arg.type = CMD_ARG_STRING; arg.sval = s;
	return arg;
}

/* ---- Registration ---- */

int cmd_register(const char *name, const char *help,
		 const char *usage, cmd_handler_t handler,
		 uint8_t min_args, uint8_t max_args)
{
	k_mutex_lock(&cmd_mutex, K_FOREVER);
	if (cmd_count >= CMD_MAX_COMMANDS) {
		k_mutex_unlock(&cmd_mutex);
		return -1;
	}
	struct cmd_entry *e = &cmd_table[cmd_count++];
	e->name = name; e->help = help; e->usage = usage;
	e->handler = handler;
	e->min_args = min_args; e->max_args = max_args;
	e->hidden = false;
	k_mutex_unlock(&cmd_mutex);
	return 0;
}

void cmd_set_output(cmd_output_fn_t fn) { cmd_output = fn; }

/* ---- Built-in Handlers ---- */

static int cmd_help_handler(int argc, struct cmd_arg *argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	cmd_print("\nAvailable commands:\n");
	cmd_print("%-16s %s\n", "Command", "Description");
	cmd_print("---------------- --------------------------------\n");
	for (int i = 0; i < cmd_count; i++) {
		if (cmd_table[i].hidden) continue;
		cmd_print("%-16s %s\n", cmd_table[i].name,
			  cmd_table[i].help ? cmd_table[i].help : "");
	}
	cmd_print("\nType '<command> --help' for usage details.\n\n");
	return 0;
}

static int cmd_status_handler(int argc, struct cmd_arg *argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	cmd_print("\n=== Command Engine Status ===\n");
	cmd_print("Registered: %d/%d\n", cmd_count, CMD_MAX_COMMANDS);
	cmd_print("Executed  : %u (ok: %u, fail: %u, unknown: %u)\n",
		  cmd_stats.total_commands, cmd_stats.successful,
		  cmd_stats.failed, cmd_stats.unknown);
	cmd_print("Arg errors: %u\n", cmd_stats.arg_errors);
	cmd_print("History   : %d/%d\n", cmd_hist.count, CMD_HISTORY_DEPTH);
	cmd_print("============================\n\n");
	return 0;
}

static int cmd_history_handler(int argc, struct cmd_arg *argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	cmd_history_dump();
	return 0;
}

static int cmd_echo_handler(int argc, struct cmd_arg *argv)
{
	for (int i = 0; i < argc; i++) {
		if (argv[i].type == CMD_ARG_INT)
			cmd_print("%d", argv[i].ival);
		else if (argv[i].type == CMD_ARG_BOOL)
			cmd_print("%s", argv[i].bval ? "true" : "false");
		else
			cmd_print("%s", argv[i].sval);
		if (i < argc - 1) cmd_print(" ");
	}
	cmd_print("\n");
	return 0;
}

static int cmd_uptime_handler(int argc, struct cmd_arg *argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	uint32_t ms = k_uptime_get_32();
	uint32_t s = ms / 1000, m = s / 60, h = m / 60;
	cmd_print("Uptime: %02u:%02u:%02u.%03u\n",
		  h, m % 60, s % 60, ms % 1000);
	return 0;
}

static int cmd_version_handler(int argc, struct cmd_arg *argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	cmd_print("ShrikeOS Monitor v1.2.0\n");
	cmd_print("Zephyr RTOS %s\n", KERNEL_VERSION_STRING);
	return 0;
}

static void cmd_register_builtins(void)
{
	cmd_register("help",    "Show available commands",
		     "help", cmd_help_handler, 0, 0);
	cmd_register("status",  "Command engine statistics",
		     "status", cmd_status_handler, 0, 0);
	cmd_register("history", "Show command history",
		     "history", cmd_history_handler, 0, 0);
	cmd_register("echo",    "Echo arguments back",
		     "echo <args...>", cmd_echo_handler, 0, CMD_MAX_ARGS);
	cmd_register("uptime",  "Show system uptime",
		     "uptime", cmd_uptime_handler, 0, 0);
	cmd_register("version", "Show firmware version",
		     "version", cmd_version_handler, 0, 0);
}

/* ---- Dispatch ---- */

static const struct cmd_entry *cmd_find(const char *name)
{
	for (int i = 0; i < cmd_count; i++) {
		const char *a = cmd_table[i].name;
		const char *b = name;
		bool match = true;
		while (*a && *b) {
			if (tolower((unsigned char)*a) !=
			    tolower((unsigned char)*b)) {
				match = false; break;
			}
			a++; b++;
		}
		if (match && *a == '\0' && *b == '\0')
			return &cmd_table[i];
	}
	return NULL;
}

int cmd_execute(char *line)
{
	if (!line || line[0] == '\0') return 0;

	while (*line && isspace((unsigned char)*line)) line++;
	size_t len = strlen(line);
	while (len > 0 && isspace((unsigned char)line[len - 1]))
		line[--len] = '\0';
	if (len == 0) return 0;

	history_add(line);

	char *tokens[CMD_MAX_ARGS + 1];
	int ntok = tokenise(line, tokens, CMD_MAX_ARGS + 1);
	if (ntok == 0) return 0;

	cmd_stats.total_commands++;

	const struct cmd_entry *entry = cmd_find(tokens[0]);
	if (!entry) {
		cmd_print("Unknown command: '%s'. Type 'help'.\n", tokens[0]);
		cmd_stats.unknown++;
		return -1;
	}

	if (ntok > 1 && strcmp(tokens[1], "--help") == 0) {
		cmd_print("Usage: %s\n", entry->usage ? entry->usage : "N/A");
		if (entry->help) cmd_print("  %s\n", entry->help);
		return 0;
	}

	int argc = ntok - 1;
	if (argc < entry->min_args) {
		cmd_print("Too few args for '%s' (min %u, got %d)\n",
			  entry->name, entry->min_args, argc);
		cmd_stats.arg_errors++;
		return -1;
	}
	if (argc > entry->max_args) {
		cmd_print("Too many args for '%s' (max %u, got %d)\n",
			  entry->name, entry->max_args, argc);
		cmd_stats.arg_errors++;
		return -1;
	}

	struct cmd_arg args[CMD_MAX_ARGS];
	for (int i = 0; i < argc; i++)
		args[i] = parse_auto(tokens[i + 1]);

	int ret = entry->handler(argc, args);
	if (ret == 0) cmd_stats.successful++;
	else { cmd_stats.failed++; }

	return ret;
}

void cmd_get_stats(uint32_t *total, uint32_t *ok, uint32_t *fail,
		   uint32_t *unknown)
{
	k_mutex_lock(&cmd_mutex, K_FOREVER);
	if (total)   *total   = cmd_stats.total_commands;
	if (ok)      *ok      = cmd_stats.successful;
	if (fail)    *fail    = cmd_stats.failed;
	if (unknown) *unknown = cmd_stats.unknown;
	k_mutex_unlock(&cmd_mutex);
}

void cmd_init(void)
{
	memset(&cmd_stats, 0, sizeof(cmd_stats));
	memset(&cmd_hist, 0, sizeof(cmd_hist));
	cmd_count = 0;
	cmd_register_builtins();
	printk("[CMD] Command engine initialised (%d built-ins)\n", cmd_count);
}
