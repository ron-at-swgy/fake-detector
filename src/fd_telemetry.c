/*
 * fd_telemetry.c -- observability surface for policy authors.
 *
 * Two telemetry facilities a policy developer reaches for while tuning:
 *
 *   fd_dump_state  -- a structured, one-fact-per-line snapshot of working
 *                     memory written to a caller-supplied stream. The
 *                     line format is the library's own (not the CLIPS
 *                     REPL's), so a harness may parse or diff it.
 *   fd_trace_*     -- mirrors CLIPS rule / fact / activation activity to
 *                     a caller stream. CLIPS routes that activity to its
 *                     STDOUT logical name, which this build installs no
 *                     console router for -- so absent a trace it is
 *                     simply discarded. fd_trace_begin installs a router
 *                     that captures it.
 *
 * fd_run_stats_get and fd_explain_suspicion -- the other two telemetry
 * entry points -- live with the read-only queries in fd_query.c.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fd_internal.h"

/* ------------------------------------------------------------------
 * fd_sink -- one dump writer, two destinations (FILE* or buffer).
 * Buffer mode keeps snprintf semantics: len accumulates the full
 * would-be output size even after the buffer fills.
 * ------------------------------------------------------------------ */

void
fd_sink_printf(struct fd_sink *s, const char *fmt, ...)
{
	va_list ap;

	if (s == NULL)
		return;
	va_start(ap, fmt);
	if (s->stream != NULL) {
		if (vfprintf(s->stream, fmt, ap) < 0)
			s->error = true;
	} else {
		size_t avail = (s->len < s->cap) ? s->cap - s->len : 0;
		char  *dst   = (avail > 0) ? s->buf + s->len : NULL;
		int n = vsnprintf(dst, avail, fmt, ap);
		if (n > 0)
			s->len += (size_t)n;
	}
	va_end(ap);
}

void
fd_dump_state(fd_detector *fd, FILE *stream)
{
	if (fd == NULL || fd->env == NULL || stream == NULL)
		return;
	fd_run(fd);
	struct fd_sink sink = { .stream = stream };
	fd_schema_dump_state(fd, &sink);
}

long
fd_dump_state_buf(fd_detector *fd, char *buf, size_t bufsize)
{
	if (fd == NULL || fd->env == NULL)
		return -1;
	if (buf == NULL && bufsize > 0)
		return -1;
	if (buf != NULL && bufsize > 0)
		buf[0] = '\0';
	fd_run(fd);
	struct fd_sink sink = { .buf = buf, .cap = bufsize };
	fd_schema_dump_state(fd, &sink);
	return (long)sink.len;
}

int
fd_dump_state_path(fd_detector *fd, const char *path)
{
	if (fd == NULL || fd->env == NULL || path == NULL)
		return -1;
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fd_run(fd);
	struct fd_sink sink = { .stream = f };
	fd_schema_dump_state(fd, &sink);
	if (fclose(f) != 0 || sink.error)
		return -1;
	return 0;
}

void
fd_ingest_stats_get(fd_detector *fd, fd_ingest_stats *out)
{
	if (out == NULL)
		return;
	*out = (fd_ingest_stats){0};
	if (fd == NULL)
		return;
	*out = fd->ingest_stats;
}

/* ------------------------------------------------------------------
 * fd_trace_* -- capturable CLIPS activity trace
 *
 * CLIPS writes (watch ...) output to its STDOUT logical name. A router
 * whose query claims that name routes the bytes to the caller's stream;
 * its query gates on trace_stream, so the router is inert once a trace
 * ends and need never be removed.
 * ------------------------------------------------------------------ */

static bool
fd_trace_query(Environment *env, const char *logical_name, void *context)
{
	fd_detector *fd = context;
	(void)env;
	return fd != NULL && fd->trace_stream != NULL &&
	    logical_name != NULL && strcmp(logical_name, STDOUT) == 0;
}

static void
fd_trace_write(Environment *env, const char *logical_name, const char *str,
    void *context)
{
	fd_detector *fd = context;
	(void)env;
	(void)logical_name;
	if (fd != NULL && fd->trace_stream != NULL && str != NULL)
		fputs(str, fd->trace_stream);
}

/* End any active trace's ownership of its stream: a stream the library
 * opened (fd_trace_begin_path) is closed; a caller stream is left
 * untouched. */
static void
trace_release_stream(fd_detector *fd)
{
	if (fd->trace_stream != NULL && fd->trace_stream_owned)
		fclose(fd->trace_stream);
	fd->trace_stream       = NULL;
	fd->trace_stream_owned = false;
}

void
fd_trace_begin(fd_detector *fd, FILE *stream)
{
	if (fd == NULL || fd->env == NULL || stream == NULL)
		return;
	trace_release_stream(fd);
	fd->trace_stream = stream;
	if (!fd->trace_router_added) {
		/* Priority 40 -- this build installs no console router, so any
		 * positive priority would do; 40 matches CLIPS' own capture
		 * routers. The router outlives the trace: its query gates on
		 * trace_stream, so it is harmless while no trace is active. */
		AddRouter(fd->env, "fd-trace", 40, fd_trace_query,
		    fd_trace_write, NULL, NULL, NULL, fd);
		fd->trace_router_added = true;
	}
	/* The activity a tuning session wants: which rules fire, on what
	 * activations, and the fact churn underneath. */
	Eval(fd->env, "(watch rules)", NULL);
	Eval(fd->env, "(watch activations)", NULL);
	Eval(fd->env, "(watch facts)", NULL);
}

int
fd_trace_begin_path(fd_detector *fd, const char *path)
{
	if (fd == NULL || fd->env == NULL || path == NULL)
		return -1;
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fd_trace_begin(fd, f);
	fd->trace_stream_owned = true;
	return 0;
}

void
fd_trace_end(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	Eval(fd->env, "(unwatch facts)", NULL);
	Eval(fd->env, "(unwatch activations)", NULL);
	Eval(fd->env, "(unwatch rules)", NULL);
	/* Clearing the stream makes fd_trace_query return false; the router
	 * stays installed but inert. A caller-supplied FILE * stays the
	 * caller's; a path-opened one is closed here. */
	trace_release_stream(fd);
}
