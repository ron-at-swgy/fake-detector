/*
 * fd_profile.c -- diagnostic / instrumentation API.
 *
 * Thin wrappers over CLIPS' built-in (profile), (profile-info), and
 * (matches) REPL commands. For perf investigation only; NOT part of
 * the stable library surface. Output flows through the CLIPS default
 * STDOUT router to the process's stdout.
 */

#include <stdio.h>

#include "fd_internal.h"

void
fd_profile_begin(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	Eval(fd->env, "(profile constructs)", NULL);
}

void
fd_profile_end(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	Eval(fd->env, "(profile off)", NULL);
}

void
fd_profile_dump(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	Eval(fd->env, "(profile-info)", NULL);
}

void
fd_profile_matches(fd_detector *fd, const char *rule_name)
{
	/* rule_name is interpolated into a CLIPS command string; reject
	 * anything that is not a plain symbol. */
	if (fd == NULL || fd->env == NULL || !fd_symbol_ok(rule_name))
		return;
	char cmd[256];
	int n = snprintf(cmd, sizeof cmd, "(matches %s)", rule_name);
	if (n < 0 || (size_t)n >= sizeof cmd)
		return;
	Eval(fd->env, cmd, NULL);
}
