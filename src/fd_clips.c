/*
 * fd_clips.c -- CLIPS interaction layer.
 *
 * The code that knows how CLIPS facts are asserted, scanned, and
 * decoded. Every other module reaches working memory through these
 * helpers rather than touching the CLIPS API directly.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fd_internal.h"

/* Elapsed wall-clock milliseconds between two CLOCK_MONOTONIC samples. */
static double
ms_between(const struct timespec *t0, const struct timespec *t1)
{
	return (double)(t1->tv_sec - t0->tv_sec) * 1000.0
	    + (double)(t1->tv_nsec - t0->tv_nsec) / 1.0e6;
}

void
fd_run(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	/* Refresh the (room-distance ...) facts if the navigation graph
	 * changed since the last run. */
	if (fd->graph.facts_dirty)
		fd_graph_publish_distances(fd);

	/* Time Run + the normalization pass together: the pair is what one
	 * fd_run costs, and fd_run_stats_get reports it. */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long long fired = Run(fd->env, -1LL);
	/* C-side arithmetic pass: convert the per-color (logodds-term)
	 * accumulators the rules just produced into normalized (suspicion)
	 * weights. Idempotent -- recomputed from immutable term facts -- so
	 * it is safe to run after every Run (every getter triggers one). */
	fd_normalize_suspicion(fd);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	/* Round-scoped instrumentation; fd_reset clears it. */
	double ms = ms_between(&t0, &t1);
	fd->run_count++;
	fd->last_rules_fired = fired;
	fd->total_rules_fired += fired;
	fd->last_run_ms = ms;
	if (ms > fd->max_run_ms)
		fd->max_run_ms = ms;
}

/* Assert a fact built by snprintf. Returns true if the fact landed in
 * working memory; false if the format overflowed the buffer or CLIPS
 * rejected the string (malformed, or a duplicate). Callers that need
 * to know an observation was lost can check the result. */
bool
fd_assert_fact(fd_detector *fd, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof buf)
		return false;
	return AssertString(fd->env, buf) != NULL;
}

/* Checked slot accessors. CLIPS' FactSlotValue returns void and, on a
 * bad slot name, yields the FALSE symbol -- a later read of the wrong
 * CLIPSValue union member is then undefined. These wrap GetFactSlot,
 * which reports a GetSlotError, and degrade to a safe sentinel (with a
 * diagnostic) instead. A failure here is a build-time bug: a slot name
 * that does not match clp/templates.clp. */
const char *
fd_fact_symbol(Fact *f, const char *slot)
{
	CLIPSValue cv;
	GetSlotError err = GetFactSlot(f, slot, &cv);
	if (err != GSE_NO_ERROR) {
		fprintf(stderr,
		    "fakedetector: symbol slot '%s' read failed (err %d)\n",
		    slot, (int)err);
		return "";
	}
	return cv.lexemeValue->contents;
}

long long
fd_fact_int(Fact *f, const char *slot)
{
	CLIPSValue cv;
	GetSlotError err = GetFactSlot(f, slot, &cv);
	if (err != GSE_NO_ERROR) {
		fprintf(stderr,
		    "fakedetector: integer slot '%s' read failed (err %d)\n",
		    slot, (int)err);
		return 0;
	}
	return cv.integerValue->contents;
}

/* True iff `s` is a non-empty CLIPS-symbol-safe string -- only the
 * characters [A-Za-z0-9_-]. Caller-supplied room and rule names are
 * checked with this before being interpolated into a CLIPS fact or
 * command string; stray whitespace, parentheses, or quotes would
 * otherwise corrupt the s-expression (and could inject facts). */
bool
fd_symbol_ok(const char *s)
{
	if (s == NULL || *s == '\0')
		return false;
	for (const char *p = s; *p != '\0'; p++) {
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

fd_stance
fd_parse_category(const char *sym)
{
	if (strcmp(sym, "on-task") == 0)  return FD_STANCE_ON_TASK;
	if (strcmp(sym, "off-task") == 0) return FD_STANCE_OFF_TASK;
	if (strcmp(sym, "threat") == 0)   return FD_STANCE_THREAT;
	return FD_STANCE_UNKNOWN;
}

fd_evidence
fd_parse_evidence(const char *sym)
{
	if (strcmp(sym, "accusation") == 0) return FD_EVIDENCE_ACCUSATION;
	if (strcmp(sym, "vent") == 0)       return FD_EVIDENCE_VENT;
	if (strcmp(sym, "near-body") == 0)  return FD_EVIDENCE_NEAR_BODY;
	if (strcmp(sym, "off-task") == 0)   return FD_EVIDENCE_OFF_TASK;
	if (strcmp(sym, "on-task") == 0)    return FD_EVIDENCE_ON_TASK;
	return FD_EVIDENCE_NONE;
}

fd_player_status
fd_parse_status(const char *sym)
{
	if (strcmp(sym, "alive") == 0)   return FD_PLAYER_STATUS_ALIVE;
	if (strcmp(sym, "dead") == 0)    return FD_PLAYER_STATUS_DEAD;
	if (strcmp(sym, "ejected") == 0) return FD_PLAYER_STATUS_EJECTED;
	return FD_PLAYER_STATUS_UNKNOWN;
}

/* Walk a single-key template, returning the first fact whose `slot`
 * SYMBOL value equals `want`. */
static Fact *
find_by_symbol(Deftemplate *tmpl, const char *slot, const char *want)
{
	for (Fact *f = GetNextFactInTemplate(tmpl, NULL);
	     f != NULL;
	     f = GetNextFactInTemplate(tmpl, f)) {
		if (strcmp(fd_fact_symbol(f, slot), want) == 0)
			return f;
	}
	return NULL;
}

Fact *
fd_find_crewmember(fd_detector *fd, fd_player who)
{
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL)
		return NULL;
	return find_by_symbol(fd->crewmember_tmpl, "color", cname);
}

Fact *
fd_find_stance(fd_detector *fd, fd_player who)
{
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL)
		return NULL;
	return find_by_symbol(fd->stance_tmpl, "color", cname);
}

Fact *
fd_find_suspicion(fd_detector *fd, fd_player who)
{
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL)
		return NULL;
	return find_by_symbol(fd->suspicion_tmpl, "color", cname);
}

Fact *
fd_find_kill_window(fd_detector *fd, const char *victim)
{
	return find_by_symbol(fd->kill_window_tmpl, "victim", victim);
}

/* The singleton (roster) fact, or NULL before the first fd_run (the
 * roster-ensure rule creates it). */
Fact *
fd_roster_fact(fd_detector *fd)
{
	if (fd == NULL || fd->roster_tmpl == NULL)
		return NULL;
	return GetNextFactInTemplate(fd->roster_tmpl, NULL);
}

/* Walk self-state facts; return the most recent (highest tick) one. */
Fact *
fd_latest_self_state(fd_detector *fd)
{
	Fact *latest = NULL;
	long long latest_tick = -1;
	for (Fact *f = GetNextFactInTemplate(fd->self_state_tmpl, NULL);
	     f != NULL;
	     f = GetNextFactInTemplate(fd->self_state_tmpl, f)) {
		long long t = fd_fact_int(f, "tick");
		if (t > latest_tick) {
			latest_tick = t;
			latest = f;
		}
	}
	return latest;
}

/* True if any (impostor-found) fact exists -- i.e. an ejection this
 * round revealed an impostor and the crewmate side has effectively won. */
bool
fd_impostor_found(fd_detector *fd)
{
	return fd->impostor_found_tmpl != NULL &&
	    GetNextFactInTemplate(fd->impostor_found_tmpl, NULL) != NULL;
}

/* True once every impostor in the round has been ejected -- the
 * multi-impostor generalization of fd_impostor_found. With the default
 * single-impostor roster the two agree. Reads the roster's
 * impostors-ejected / n-impostors slots, which roster-recount maintains. */
bool
fd_all_impostors_ejected(fd_detector *fd)
{
	Fact *r = fd_roster_fact(fd);
	if (r == NULL)
		return false;
	long long ni = fd_fact_int(r, "n-impostors");
	long long ne = fd_fact_int(r, "impostors-ejected");
	return ni >= 1 && ne >= ni;
}

int
fd_count_facts(Deftemplate *tmpl)
{
	int n = 0;
	for (Fact *f = GetNextFactInTemplate(tmpl, NULL);
	     f != NULL;
	     f = GetNextFactInTemplate(tmpl, f)) {
		n++;
	}
	return n;
}

/* Integer logistic: map a log-odds value in milli-nats (1000 = 1.0 nat)
 * to a per-mille probability 0..1000. A 17-point lookup table over
 * [-8000, 8000] milli-nats with integer linear interpolation between
 * points. Only monotonicity and the endpoint behavior matter -- a
 * cast-iron -8000 maps to ~0, an observed-vent +3000 maps high. The
 * table is logistic(n nats) * 1000 for n = -8 .. +8.
 *
 * Exposed (via fd_internal.h) so fd_explain_suspicion can report the
 * pre-normalization likelihood with the exact mapping fd_run uses. */
int
fd_logistic_permille(long long milli_nats)
{
	static const int TBL[17] = {
		   0,   1,   2,   7,  18,  47, 119, 269, 500,
		 731, 881, 953, 982, 993, 998, 999, 1000
	};
	long long x = milli_nats;
	if (x < -8000)
		x = -8000;
	if (x > 8000)
		x = 8000;
	long long shifted = x + 8000;           /* 0 .. 16000 */
	int idx  = (int)(shifted / 1000);        /* 0 .. 16   */
	int frac = (int)(shifted % 1000);        /* 0 .. 999  */
	if (idx >= 16)
		return TBL[16];
	return TBL[idx] + (TBL[idx + 1] - TBL[idx]) * frac / 1000;
}

/* Convert the per-color (logodds-term) accumulators into normalized
 * (suspicion) weights. This is the DeepRole renormalization step, done
 * C-side (CLIPS decides WHO gets which logodds; C only divides). It is
 * idempotent: every call recomputes from scratch out of the immutable
 * (logodds-term) facts and overwrites the suspicion slots, so running
 * it after every Run is correct and cheap.
 *
 * weight values sum to 1000 across the alive non-self pool; clearing a
 * color (its suspicion fact retracted, or its logodds floored by a
 * cast-iron alibi) mechanically raises every survivor's weight. */
void
fd_normalize_suspicion(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL ||
	    fd->logodds_term_tmpl == NULL || fd->suspicion_tmpl == NULL)
		return;

	/* 1. sum logodds-term.amount per color. */
	long long sum[FD_MAX_PLAYERS];
	for (int i = 0; i < FD_MAX_PLAYERS; i++)
		sum[i] = 0;
	for (Fact *t = GetNextFactInTemplate(fd->logodds_term_tmpl, NULL);
	     t != NULL;
	     t = GetNextFactInTemplate(fd->logodds_term_tmpl, t)) {
		int ci = fd_player_from_name(fd, fd_fact_symbol(t, "color"));
		if (ci < 0)
			continue;
		sum[ci] += fd_fact_int(t, "amount");
	}

	/* 2. collect every suspicion fact, compute its logistic likelihood.
	 *    Pointers are gathered before any modification -- FMModify
	 *    invalidates the fact it modifies, but never its siblings. */
	Fact     *facts[FD_MAX_PLAYERS];
	int       fc[FD_MAX_PLAYERS];
	long long lik[FD_MAX_PLAYERS];
	long long remainder[FD_MAX_PLAYERS];
	long long weight[FD_MAX_PLAYERS];
	int       n = 0;
	long long total = 0;
	for (Fact *s = GetNextFactInTemplate(fd->suspicion_tmpl, NULL);
	     s != NULL && n < FD_MAX_PLAYERS;
	     s = GetNextFactInTemplate(fd->suspicion_tmpl, s)) {
		int ci = fd_player_from_name(fd, fd_fact_symbol(s, "color"));
		if (ci < 0)
			continue;
		facts[n] = s;
		fc[n]    = ci;
		lik[n]   = fd_logistic_permille(sum[ci]);
		total   += lik[n];
		n++;
	}
	for (int i = 1; i < n; i++) {
		Fact *fact_key = facts[i];
		int fc_key = fc[i];
		long long lik_key = lik[i];
		int j = i - 1;
		while (j >= 0 && fc[j] > fc_key) {
			facts[j + 1] = facts[j];
			fc[j + 1] = fc[j];
			lik[j + 1] = lik[j];
			j--;
		}
		facts[j + 1] = fact_key;
		fc[j + 1] = fc_key;
		lik[j + 1] = lik_key;
	}

	/* 3. compute exact per-mille weights with largest-remainder
	 * rounding. Ties break by color enum order because fc[] is sorted
	 * ascending above. */
	long long assigned = 0;
	if (total > 0) {
		for (int i = 0; i < n; i++) {
			long long scaled = lik[i] * 1000;
			weight[i] = scaled / total;
			remainder[i] = scaled % total;
			assigned += weight[i];
		}
	} else if (n > 0) {
		long long base = 1000 / n;
		long long extra = 1000 % n;
		for (int i = 0; i < n; i++) {
			weight[i] = base + (i < extra ? 1 : 0);
			remainder[i] = 0;
			assigned += weight[i];
		}
	}
	for (long long left = 1000 - assigned; total > 0 && left > 0; left--) {
		int best = 0;
		for (int i = 1; i < n; i++) {
			if (remainder[i] > remainder[best] ||
			    (remainder[i] == remainder[best] && fc[i] < fc[best]))
				best = i;
		}
		weight[best]++;
		remainder[best] = -1;
	}

	/* 4. write logodds + normalized weight back into each fact. */
	for (int i = 0; i < n; i++) {
		FactModifier *fm = CreateFactModifier(fd->env, facts[i]);
		if (fm == NULL)
			continue;
		FMPutSlotInteger(fm, "logodds", sum[fc[i]]);
		FMPutSlotInteger(fm, "weight",  weight[i]);
		FMModify(fm);
		FMDispose(fm);
	}
}
