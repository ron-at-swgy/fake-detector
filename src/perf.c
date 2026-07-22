/*
 * perf.c - performance test harness for libfakedetector.
 *
 * Drives three elaborate scenarios through the library, times every
 * public-API call, and exits non-zero if any post-warmup call exceeds
 * its kind's budget. fd_create is excluded — startup is one-time
 * Rete-network construction and is not a per-call latency signal.
 *
 * Budgets (two kinds, both checked per-call):
 *   assert kind: fd_set_self, fd_observe_*, fd_reset
 *                FD_PERF_ASSERT_BUDGET_NS (default  5 ms)
 *   query kind:  fd_run, fd_render_*, fd_pick_vote_target,
 *                fd_get_*, fd_alone_risk*
 *                FD_PERF_QUERY_BUDGET_NS  (default 10 ms)
 *
 * Scenarios (self = RED):
 *   warmup  small priming scenario; samples recorded but no failures
 *           (the first query pays the Rete first-walk cost).
 *   A       multi-body cascade across three sub-rounds with fd_reset
 *           between each. 16 colors, ~75 observations, 3 vote rounds.
 *   C       long single round: 1000+ fact assertions interleaved with
 *           safety checks and votes. Stresses query latency against
 *           a large working-memory population.
 *   B       vent-storm: four impossible-movement pairs from cafeteria
 *           to room X with Δt=25 ticks (~1 sec) against link costs of
 *           50-100 ticks. Two bodies, one non-impostor and one
 *           impostor ejection. Runs LAST so its asserted navigation
 *           graph doesn't leak into A or C — neither rely on vents.
 *
 * Overrides:
 *   compile: gmake perf CPPFLAGS+='-DFD_PERF_QUERY_BUDGET_NS=20000000L'
 *   runtime: FD_PERF_QUERY_BUDGET_NS=20000000 \
 *            FD_PERF_ASSERT_BUDGET_NS=10000000 ./build/perf
 */

/* clock_gettime / CLOCK_MONOTONIC are POSIX; the project-wide
 * _POSIX_C_SOURCE is supplied by the build (APP_POSIX in mk/sources.mk),
 * not a per-file #define. */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fakedetector.h"

#if defined(HAVE_PLEDGE) || defined(HAVE_UNVEIL)
#  include <unistd.h>
#endif

/* Harness-local player ids (legacy Among Them roster order). Names are
 * left at the library's "p<id>" defaults — nothing here prints them. */
enum {
	P_RED = 0, P_ORANGE, P_YELLOW, P_LIGHT_BLUE,
	P_PINK, P_LIME, P_BLUE, P_PALE_BLUE,
	P_GRAY, P_WHITE, P_DARK_BROWN, P_BROWN,
	P_DARK_TEAL, P_GREEN, P_DARK_NAVY, P_BLACK,
	P__COUNT
};

#ifndef FD_PERF_ASSERT_BUDGET_NS
#define FD_PERF_ASSERT_BUDGET_NS  5000000  /* 5 ms (assert-side calls) */
#endif
#ifndef FD_PERF_QUERY_BUDGET_NS
#define FD_PERF_QUERY_BUDGET_NS  10000000  /* 10 ms (query/render calls) */
#endif

#define MAX_SAMPLES    2048
#define MAX_CALL_TYPES   32

/* Timing values are int64_t throughout: a nanosecond count overflows
 * a 32-bit long after ~2 s, which would silently corrupt measurements
 * on any ILP32 target. */
typedef struct {
	const char *name;
	int         count;
	int64_t     samples[MAX_SAMPLES];
} call_stats;

static call_stats  g_stats[MAX_CALL_TYPES];
static int         g_n_stats = 0;
static int         g_warmup = 1;
static int         g_failures = 0;
static int64_t     g_assert_budget = FD_PERF_ASSERT_BUDGET_NS;
static int64_t     g_query_budget  = FD_PERF_QUERY_BUDGET_NS;
static int         g_profile = 0;   /* FD_PERF_PROFILE=1 to enable */
static const char *g_scenario = "";
static int         g_scenario_asserts = 0;
static int         g_scenario_queries = 0;

/* The assert kind: state-mutating calls that just push facts/clear WM.
 * Anything else is a query (implicitly runs the rule engine). */
static int
is_assert_call(const char *name)
{
	static const char *const asserts[] = {
		"fd_set_self",
		"fd_observe_self",
		"fd_observe_player",
		"fd_observe_body",
		"fd_observe_task_completion",
		"fd_observe_death",
		"fd_observe_ejection",
		"fd_observe_round_config",
		"fd_observe_claim",
		"fd_observe_vote",
		"fd_observe_defense",
		"fd_reset",
		NULL
	};
	for (int i = 0; asserts[i] != NULL; i++) {
		if (strcmp(name, asserts[i]) == 0)
			return 1;
	}
	return 0;
}

static int64_t
budget_for(const char *name)
{
	return is_assert_call(name) ? g_assert_budget : g_query_budget;
}

static call_stats *
get_stat(const char *name)
{
	for (int i = 0; i < g_n_stats; i++) {
		if (strcmp(g_stats[i].name, name) == 0)
			return &g_stats[i];
	}
	if (g_n_stats >= MAX_CALL_TYPES) {
		fprintf(stderr, "perf: MAX_CALL_TYPES exhausted\n");
		exit(2);
	}
	g_stats[g_n_stats].name = name;
	g_stats[g_n_stats].count = 0;
	return &g_stats[g_n_stats++];
}

static void
record_sample(const char *name, int64_t ns)
{
	call_stats *s = get_stat(name);
	if (s->count < MAX_SAMPLES)
		s->samples[s->count] = ns;
	s->count++;
	if (is_assert_call(name))
		g_scenario_asserts++;
	else
		g_scenario_queries++;
	int64_t bud = budget_for(name);
	if (!g_warmup && ns > bud) {
		fprintf(stderr,
		    "OVERRUN [%s] %s call#%d: %" PRId64 " ns "
		    "(budget %" PRId64 " ns)\n",
		    g_scenario, name, s->count, ns, bud);
		g_failures++;
	}
}

#define TIME_CALL(name, expr) do {                                          \
	struct timespec t0_, t1_;                                            \
	clock_gettime(CLOCK_MONOTONIC, &t0_);                                \
	(expr);                                                              \
	clock_gettime(CLOCK_MONOTONIC, &t1_);                                \
	int64_t ns_ = (int64_t)(t1_.tv_sec - t0_.tv_sec) * 1000000000 +      \
	              (t1_.tv_nsec - t0_.tv_nsec);                           \
	record_sample((name), ns_);                                          \
} while (0)

static int
cmp_i64(const void *a, const void *b)
{
	int64_t la = *(const int64_t *)a;
	int64_t lb = *(const int64_t *)b;
	return (la > lb) - (la < lb);
}

static void
print_summary(void)
{
	printf("\n%-28s %4s %6s %10s %10s %10s %10s\n",
	    "call", "kind", "n", "min", "median", "max", "budget");
	printf("%-28s %4s %6s %10s %10s %10s %10s\n",
	    "----", "----", "-", "---", "------", "---", "------");
	for (int i = 0; i < g_n_stats; i++) {
		call_stats *s = &g_stats[i];
		int n = s->count > MAX_SAMPLES ? MAX_SAMPLES : s->count;
		if (n == 0)
			continue;
		int64_t sorted[MAX_SAMPLES];
		memcpy(sorted, s->samples, sizeof sorted[0] * (size_t)n);
		qsort(sorted, (size_t)n, sizeof sorted[0], cmp_i64);
		int64_t mn  = sorted[0];
		int64_t mx  = sorted[n - 1];
		int64_t med = sorted[n / 2];
		const char *kind = is_assert_call(s->name) ? "asrt" : "qury";
		printf("%-28s %4s %6d %7" PRId64 " us %7" PRId64 " us "
		    "%7" PRId64 " us %7" PRId64 " us\n",
		    s->name, kind, s->count,
		    mn / 1000, med / 1000, mx / 1000,
		    budget_for(s->name) / 1000);
	}
}

static void
parse_env_one(const char *var, int64_t *out)
{
	const char *e = getenv(var);
	if (e == NULL || *e == '\0')
		return;
	char *end;
	long long v = strtoll(e, &end, 10);
	if (end != e && *end == '\0' && v > 0) {
		*out = v;
		printf("budget override: %s=%" PRId64 "\n", var, (int64_t)v);
	}
}

static void
parse_env_budgets(void)
{
	parse_env_one("FD_PERF_ASSERT_BUDGET_NS", &g_assert_budget);
	parse_env_one("FD_PERF_QUERY_BUDGET_NS",  &g_query_budget);

	const char *p = getenv("FD_PERF_PROFILE");
	if (p != NULL && *p != '\0' && *p != '0') {
		g_profile = 1;
		printf("profile: FD_PERF_PROFILE=%s -- per-rule timing "
		    "will be dumped at end\n", p);
	}
}

static void
scenario_begin(const char *name)
{
	g_scenario = name;
	g_scenario_asserts = 0;
	g_scenario_queries = 0;
}

static void
scenario_end(void)
{
	printf("scenario %s: %d asserts, %d queries\n",
	    g_scenario, g_scenario_asserts, g_scenario_queries);
}

/* ------------------------------------------------------------------
 * Warmup: priming scenario so the first measured query doesn't pay
 * Rete-network first-walk cost. Samples are recorded but no failures
 * are raised.
 * ------------------------------------------------------------------ */
static void
run_warmup(fd_detector *fd)
{
	scenario_begin("warmup");
	g_warmup = 1;

	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));
	for (int c = P_ORANGE; c <= P_LIME; c++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, 10, (fd_player)c,
		        "cafeteria", 60 + c * 5, 60));
	}
	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 30, P_BLUE, "admin"));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 80, P_LIME, "storage", 30, 100));
	TIME_CALL("fd_render_vote_summary",
	    (void)fd_render_vote_summary(fd, buf, sizeof buf));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_alone_risk",
	    (void)fd_alone_risk(fd, P_ORANGE));

	TIME_CALL("fd_reset", fd_reset(fd));
	g_warmup = 0;
	scenario_end();
}

/* ------------------------------------------------------------------
 * Scenario A: multi-body cascade across three sub-rounds.
 *   Sub-round 1: body of GREEN in storage; vote + 15 alone-risks.
 *   Sub-round 2: fd_reset, body of BROWN in medbay; vote + 13 risks.
 *   Sub-round 3: fd_reset, body of PALE_BLUE in admin; vote + 11
 *                risks + impostor-ejection cascade + re-query.
 * ------------------------------------------------------------------ */
/* Sub-round 1 query batch: render, vote-pick, 15 alone-risks, spot
 * reads -- the timed-query half of sub-round 1. */
static void
scenario_a_round1_queries(fd_detector *fd)
{
	char     buf[2048];
	char     rbuf[256];
	fd_player vc;
	fd_stance vs;

	TIME_CALL("fd_render_vote_summary",
	    (void)fd_render_vote_summary(fd, buf, sizeof buf));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	for (int c = 0; c < P__COUNT; c++) {
		if (c == P_RED)
			continue;
		TIME_CALL("fd_alone_risk",
		    (void)fd_alone_risk(fd, (fd_player)c));
	}
	TIME_CALL("fd_get_stance", (void)fd_get_stance(fd, P_PINK));
	TIME_CALL("fd_get_stance", (void)fd_get_stance(fd, P_YELLOW));
	TIME_CALL("fd_get_status", (void)fd_get_status(fd, P_GREEN));
	TIME_CALL("fd_get_rationale",
	    (void)fd_get_rationale(fd, P_PINK, rbuf, sizeof rbuf));
	TIME_CALL("fd_alone_risk_rationale",
	    (void)fd_alone_risk_rationale(fd, P_YELLOW,
	        rbuf, sizeof rbuf));

	/* Probabilistic-layer queries (Enh. 1 / 2 / 6). */
	for (int c = 0; c < P__COUNT; c++) {
		if (c == P_RED)
			continue;
		TIME_CALL("fd_get_suspicion",
		    (void)fd_get_suspicion(fd, (fd_player)c));
	}
	TIME_CALL("fd_game_pressure", (void)fd_game_pressure(fd));
	TIME_CALL("fd_win_probability", (void)fd_win_probability(fd));
	{
		fd_vote_decision_t vd;
		TIME_CALL("fd_vote_decision", (void)fd_vote_decision(fd, &vd));
	}
}

/* Sub-round 1: feed the round's observations, then run the query
 * batch. GREEN dies in storage; YELLOW trips a vent. */
static void
scenario_a_round1(fd_detector *fd)
{
	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_round_config",
	    fd_observe_round_config(fd, 16, 2));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));

	for (int c = P_ORANGE; c < P__COUNT; c++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, 10, (fd_player)c, "cafeteria",
		        60 + c, 60 + (c % 3) * 5));
	}

	int ttick[3] = { 30, 50, 70 };
	for (int i = 0; i < 3; i++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, ttick[i], P_BLUE,
		        "admin", 100, 40));
		TIME_CALL("fd_observe_task_completion",
		    fd_observe_task_completion(fd, ttick[i], P_BLUE,
		        "admin"));
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, ttick[i], P_GREEN,
		        "electrical", 50, 80));
		TIME_CALL("fd_observe_task_completion",
		    fd_observe_task_completion(fd, ttick[i], P_GREEN,
		        "electrical"));
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, ttick[i], P_BLACK,
		        "medbay", 20, 90));
		TIME_CALL("fd_observe_task_completion",
		    fd_observe_task_completion(fd, ttick[i], P_BLACK,
		        "medbay"));
	}
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 40, P_LIME, "cafeteria", 88, 64));
	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 40, P_LIME, "cafeteria"));

	/* PINK lurks near storage; YELLOW impossible move (vent flag). */
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 60, P_PINK, "storage", 30, 100));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 75, P_PINK, "storage", 32, 102));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 50, P_YELLOW, "cafeteria", 64, 64));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 53, P_YELLOW, "admin", 220, 60));

	/* Body of GREEN in storage @ t=80. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 80, FD_PHASE_PLAYING, "storage", 28, 98, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 80, P_GREEN, "storage", 35, 105));

	/* Post-body alibi sightings. */
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 85, P_BLUE, "admin", 100, 40));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 90, P_BLACK, "medbay", 20, 90));

	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 100, FD_PHASE_VOTING, "cafeteria", 64, 64, 0));

	/* Meeting chat: claims, votes, a defense (Enh. 3 / 5). */
	TIME_CALL("fd_observe_claim",
	    fd_observe_claim(fd, 60, P_PINK, FD_CLAIM_LOCATION, "admin"));
	TIME_CALL("fd_observe_claim",
	    fd_observe_claim(fd, 80, P_BLACK, FD_CLAIM_SELF_REPORT,
	        "storage"));
	TIME_CALL("fd_observe_vote",
	    fd_observe_vote(fd, 100, P_BLUE, P_PINK));
	TIME_CALL("fd_observe_vote",
	    fd_observe_vote(fd, 100, P_LIME, P_PINK));
	TIME_CALL("fd_observe_defense",
	    fd_observe_defense(fd, 100, P_GREEN, P_PINK));

	scenario_a_round1_queries(fd);
}

/* Sub-round 2: fd_reset, body of BROWN in medbay. */
static void
scenario_a_round2(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	TIME_CALL("fd_reset", fd_reset(fd));
	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 120, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));

	fd_player sub2[] = {
		P_ORANGE, P_YELLOW, P_LIGHT_BLUE,
		P_PINK, P_LIME, P_BLUE,
		P_PALE_BLUE, P_GRAY, P_WHITE,
		P_DARK_BROWN, P_BROWN, P_DARK_TEAL,
		P_DARK_NAVY, P_BLACK,
	};
	size_t sub2n = sizeof(sub2) / sizeof(sub2[0]);
	for (size_t i = 0; i < sub2n; i++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, 125, sub2[i],
		        (i % 2 == 0) ? "medbay" : "reactor",
		        30 + (int)i * 3, 80));
	}
	for (int i = 0; i < 3; i++) {
		TIME_CALL("fd_observe_task_completion",
		    fd_observe_task_completion(fd, 140 + i * 5,
		        P_LIGHT_BLUE, "admin"));
	}
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 160, P_GRAY, "medbay", 25, 92));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 175, P_GRAY, "medbay", 27, 95));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 180, FD_PHASE_PLAYING, "medbay", 22, 88, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 180, P_BROWN, "medbay", 28, 94));

	TIME_CALL("fd_render_vote_summary",
	    (void)fd_render_vote_summary(fd, buf, sizeof buf));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	for (size_t i = 0; i < sub2n; i++) {
		if (sub2[i] == P_BROWN)
			continue;
		TIME_CALL("fd_alone_risk",
		    (void)fd_alone_risk(fd, sub2[i]));
	}
}

/* Sub-round 3: fd_reset, body of PALE_BLUE, impostor-ejection cascade. */
static void
scenario_a_round3(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	TIME_CALL("fd_reset", fd_reset(fd));
	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 200, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));

	fd_player sub3[] = {
		P_ORANGE, P_YELLOW, P_PINK, P_LIME,
		P_BLUE, P_PALE_BLUE, P_GRAY, P_WHITE,
		P_DARK_BROWN, P_DARK_TEAL, P_DARK_NAVY,
		P_BLACK,
	};
	size_t sub3n = sizeof(sub3) / sizeof(sub3[0]);
	for (size_t i = 0; i < sub3n; i++) {
		const char *room =
		    (i % 3 == 0) ? "admin" :
		    (i % 3 == 1) ? "reactor" : "electrical";
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, 210, sub3[i], room,
		        40 + (int)i * 4, 50));
	}
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 230, P_BLACK, "admin", 95, 38));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 240, FD_PHASE_PLAYING, "admin", 88, 42, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 240, P_PALE_BLUE, "admin", 96, 44));

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	for (size_t i = 0; i < sub3n; i++) {
		if (sub3[i] == P_PALE_BLUE)
			continue;
		TIME_CALL("fd_alone_risk",
		    (void)fd_alone_risk(fd, sub3[i]));
	}
	TIME_CALL("fd_observe_ejection",
	    fd_observe_ejection(fd, 260, P_BLACK, 1));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
}

static void
run_scenario_a(fd_detector *fd)
{
	scenario_begin("A");
	scenario_a_round1(fd);
	scenario_a_round2(fd);
	scenario_a_round3(fd);
	scenario_end();
}

/* ------------------------------------------------------------------
 * Scenario B: vent storm + ejection cascade.
 * ------------------------------------------------------------------ */
struct vent_pair {
	fd_player    c;
	int         t1, t2;
	const char *r1, *r2;
	int         x1, y1, x2, y2;
};

/* Graph, the 16-color cluster, and the four impossible-movement
 * pairs. Each pair starts from cafeteria (matching the cluster, so
 * the prior-sighting check is same-room = no spurious vent) and
 * jumps to room X at Δt=25 against link costs of 50-100. */
static void
scenario_b_setup(fd_detector *fd)
{
	TIME_CALL("fd_reset", fd_reset(fd));
	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));

	fd_add_room_link(fd, "cafeteria", "medbay",  50);
	fd_add_room_link(fd, "cafeteria", "storage", 60);
	fd_add_room_link(fd, "cafeteria", "admin",   80);
	fd_add_room_link(fd, "cafeteria", "reactor", 100);

	for (int c = P_ORANGE; c < P__COUNT; c++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, 10, (fd_player)c, "cafeteria",
		        60 + c * 2, 60 + (c % 4) * 3));
	}

	struct vent_pair pairs[] = {
		{ P_YELLOW,    20, 45, "cafeteria", "reactor",
		    64, 64, 300, 200 },
		{ P_BROWN,     30, 55, "cafeteria", "admin",
		    64, 64, 280,  60 },
		{ P_DARK_TEAL, 40, 65, "cafeteria", "storage",
		    64, 64, 290, 110 },
		{ P_GRAY,      50, 75, "cafeteria", "medbay",
		    64, 64,  20,  90 },
	};
	size_t np = sizeof(pairs) / sizeof(pairs[0]);
	for (size_t i = 0; i < np; i++) {
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, pairs[i].t1, pairs[i].c,
		        pairs[i].r1, pairs[i].x1, pairs[i].y1));
		TIME_CALL("fd_observe_player",
		    fd_observe_player(fd, pairs[i].t2, pairs[i].c,
		        pairs[i].r2, pairs[i].x2, pairs[i].y2));
	}

	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 60, P_BLUE, "admin"));
	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 65, P_BLUE, "admin"));
	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 70, P_WHITE, "electrical"));
	TIME_CALL("fd_observe_task_completion",
	    fd_observe_task_completion(fd, 75, P_WHITE, "electrical"));
}

/* Two bodies, the interleaved votes/risks, and the non-impostor then
 * impostor ejection cascade. */
static void
scenario_b_bodies(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	/* Body of ORANGE in cafeteria @ t=120. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 120, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 120, P_ORANGE, "cafeteria", 70, 60));

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_YELLOW));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_BROWN));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_BLUE));

	/* More sightings narrowing the second kill window. */
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 140, P_PINK, "storage", 30, 100));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 150, P_LIME, "medbay", 22, 92));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 155, P_LIGHT_BLUE, "medbay", 24, 90));
	TIME_CALL("fd_observe_player",
	    fd_observe_player(fd, 170, P_DARK_NAVY, "medbay", 28, 88));

	/* Body of BLACK in medbay @ t=180. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 180, FD_PHASE_PLAYING, "medbay", 22, 88, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 180, P_BLACK, "medbay", 30, 90));

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_render_vote_summary",
	    (void)fd_render_vote_summary(fd, buf, sizeof buf));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_DARK_TEAL));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_GRAY));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_WHITE));

	/* Non-impostor ejection — threats survive. */
	TIME_CALL("fd_observe_ejection",
	    fd_observe_ejection(fd, 200, P_YELLOW, 0));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));

	/* Impostor ejection — clear-threats-on-impostor-found fires. */
	TIME_CALL("fd_observe_ejection",
	    fd_observe_ejection(fd, 220, P_BROWN, 1));
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
}

static void
run_scenario_b(fd_detector *fd)
{
	scenario_begin("B");
	scenario_b_setup(fd);
	scenario_b_bodies(fd);
	scenario_end();
}

/* ------------------------------------------------------------------
 * Scenario C: long round, 1000+ fact assertions interleaved with
 * safety checks and votes. Realistic shape: three bodies spread
 * across a round of dense sightings.
 *
 *   Phase 1 (t=10..79  step 3):  ~360 sightings (dossier warm-up).
 *                                  60 task completions.
 *                                  5 alone-risks mid-phase.
 *   Body 1 (t=80):               GREEN dies in storage.
 *                                  vote + render_case + 7 alone-risks.
 *   Phase 3 (t=85..129 step 3):  ~210 sightings.
 *                                  5 alone-risks.
 *   Body 2 (t=130):              BROWN dies in medbay.
 *                                  vote + render_case.
 *   Phase 5 (t=135..179 step 3): ~195 sightings + 36 task completions.
 *   Body 3 (t=180):              PALE_BLUE dies in admin.
 *                                  vote + render_case + render_vote_summary.
 *                                  ejection of GRAY (non-impostor).
 *   Phase 7 (t=200..219 step 2): ~220 sightings.
 *                                  final vote + 11 alone-risks.
 *
 * Known to exceed the 10 ms query budget on body 3 — the post-body
 * cascade against ~789 accumulated sightings consistently lands
 * around 13–15 ms in measurements on this host. The test will FAIL
 * here until the underlying perf issue is fixed. Do not "fix" this
 * by reshaping the scenario.
 * ------------------------------------------------------------------ */
/* Room cycle and tasker roster shared by scenario C's phase helpers. */
static const char *const SCENARIO_C_ROOMS[] = {
	"cafeteria", "medbay", "reactor", "admin", "storage", "electrical",
};
static const int SCENARIO_C_N_ROOMS =
    (int)(sizeof SCENARIO_C_ROOMS / sizeof SCENARIO_C_ROOMS[0]);

static const fd_player SCENARIO_C_TASKERS[] = {
	P_BLUE, P_GREEN, P_BLACK, P_WHITE,
	P_DARK_BROWN, P_DARK_TEAL, P_DARK_NAVY,
	P_LIGHT_BLUE, P_LIME, P_PALE_BLUE,
};
static const size_t SCENARIO_C_N_TASKERS =
    sizeof SCENARIO_C_TASKERS / sizeof SCENARIO_C_TASKERS[0];

/* Phase 1: dossier warm-up (t=10..79), task completions, 5 alone-risks,
 * then body 1 (GREEN) and its query batch. */
static void
scenario_c_phase1(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	TIME_CALL("fd_reset", fd_reset(fd));
	TIME_CALL("fd_set_self", fd_set_self(fd, P_RED));
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0));

	for (int t = 10; t <= 79; t += 3) {
		for (int c = P_ORANGE; c < P__COUNT; c++) {
			int ri = (c + t / 15) % SCENARIO_C_N_ROOMS;
			int x  = 50 + ((c * 7 + t) % 200);
			int y  = 50 + ((c * 11 + t) % 100);
			TIME_CALL("fd_observe_player",
			    fd_observe_player(fd, t, (fd_player)c,
			        SCENARIO_C_ROOMS[ri], x, y));
		}
	}
	for (int t = 20; t <= 70; t += 10) {
		for (size_t i = 0; i < SCENARIO_C_N_TASKERS; i++) {
			TIME_CALL("fd_observe_task_completion",
			    fd_observe_task_completion(fd, t,
			        SCENARIO_C_TASKERS[i], "admin"));
		}
	}
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_ORANGE));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_YELLOW));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_PINK));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_GRAY));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_BROWN));

	/* Body 1: GREEN @ t=80. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 80, FD_PHASE_PLAYING, "storage", 30, 105, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 80, P_GREEN, "storage", 35, 110));

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	for (int c = P_ORANGE; c < P_PALE_BLUE; c++) {
		TIME_CALL("fd_alone_risk",
		    (void)fd_alone_risk(fd, (fd_player)c));
	}
}

/* Phase 2: sightings (t=85..129), 5 alone-risks, body 2 (BROWN) and
 * its queries, then more sightings + tasks (t=135..179). */
static void
scenario_c_phase2(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	for (int t = 85; t <= 129; t += 3) {
		for (int c = P_ORANGE; c < P__COUNT; c++) {
			if (c == P_GREEN)
				continue;
			int ri = (c + t / 17) % SCENARIO_C_N_ROOMS;
			int x  = 60 + ((c * 5 + t) % 200);
			int y  = 60 + ((c * 13 + t) % 100);
			TIME_CALL("fd_observe_player",
			    fd_observe_player(fd, t, (fd_player)c,
			        SCENARIO_C_ROOMS[ri], x, y));
		}
	}
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_PINK));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_YELLOW));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_BROWN));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_DARK_TEAL));
	TIME_CALL("fd_alone_risk", (void)fd_alone_risk(fd, P_GRAY));

	/* Body 2: BROWN @ t=130. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 130, FD_PHASE_PLAYING, "medbay", 22, 88, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 130, P_BROWN, "medbay", 28, 92));

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));

	for (int t = 135; t <= 179; t += 3) {
		for (int c = P_ORANGE; c < P__COUNT; c++) {
			if (c == P_GREEN || c == P_BROWN)
				continue;
			int ri = (c + t / 13) % SCENARIO_C_N_ROOMS;
			int x  = 70 + ((c * 9 + t) % 180);
			int y  = 70 + ((c * 7 + t) % 100);
			TIME_CALL("fd_observe_player",
			    fd_observe_player(fd, t, (fd_player)c,
			        SCENARIO_C_ROOMS[ri], x, y));
		}
	}
	for (int t = 140; t <= 170; t += 10) {
		for (size_t i = 0; i < SCENARIO_C_N_TASKERS; i++) {
			if (SCENARIO_C_TASKERS[i] == P_GREEN)
				continue;
			TIME_CALL("fd_observe_task_completion",
			    fd_observe_task_completion(fd, t,
			        SCENARIO_C_TASKERS[i], "electrical"));
		}
	}
}

/* Dump (matches) for the body-3 hotspot rules. Diagnostic only. */
static void
scenario_c_dump_matches(fd_detector *fd)
{
	fputs("\n--- (matches) after body 3, before pick_vote ---\n", stdout);
	fflush(stdout);
	fd_profile_matches(fd, "derive-alibi");
	fd_profile_matches(fd, "open-suspect-from-crewmember");
	fd_profile_matches(fd, "stance-threat");
	fd_profile_matches(fd, "update-kill-window-lower");
	fputs("--- end matches ---\n", stdout);
	fflush(stdout);
}

/* Phase 3: body 3 (PALE_BLUE), the slow post-body query batch, the
 * GRAY ejection, the final stretch (t=200..219), final vote + risks. */
static void
scenario_c_phase3(fd_detector *fd)
{
	char     buf[2048];
	fd_player vc;
	fd_stance vs;

	/* Body 3: PALE_BLUE @ t=180. */
	TIME_CALL("fd_observe_self",
	    fd_observe_self(fd, 180, FD_PHASE_PLAYING, "admin", 90, 42, 0));
	TIME_CALL("fd_observe_body",
	    fd_observe_body(fd, 180, P_PALE_BLUE, "admin", 96, 44));

	if (g_profile)
		scenario_c_dump_matches(fd);

	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	TIME_CALL("fd_render_case",
	    (void)fd_render_case(fd, buf, sizeof buf));
	TIME_CALL("fd_render_vote_summary",
	    (void)fd_render_vote_summary(fd, buf, sizeof buf));
	TIME_CALL("fd_observe_ejection",
	    fd_observe_ejection(fd, 199, P_GRAY, 0));

	for (int t = 200; t <= 219; t += 1) {
		for (int c = P_ORANGE; c < P__COUNT; c++) {
			if (c == P_GREEN  || c == P_BROWN ||
			    c == P_PALE_BLUE || c == P_GRAY)
				continue;
			int ri = (c + t / 11) % SCENARIO_C_N_ROOMS;
			int x  = 80 + ((c * 6 + t) % 180);
			int y  = 80 + ((c * 8 + t) % 100);
			TIME_CALL("fd_observe_player",
			    fd_observe_player(fd, t, (fd_player)c,
			        SCENARIO_C_ROOMS[ri], x, y));
		}
	}
	TIME_CALL("fd_pick_vote_target",
	    (void)fd_pick_vote_target(fd, &vc, &vs));
	for (int c = P_ORANGE; c < P__COUNT; c++) {
		if (c == P_GREEN  || c == P_BROWN ||
		    c == P_PALE_BLUE || c == P_GRAY)
			continue;
		TIME_CALL("fd_alone_risk",
		    (void)fd_alone_risk(fd, (fd_player)c));
	}
}

static void
run_scenario_c(fd_detector *fd)
{
	scenario_begin("C");
	scenario_c_phase1(fd);
	scenario_c_phase2(fd);
	scenario_c_phase3(fd);
	scenario_end();
}

/* OpenBSD sandbox -- no-ops elsewhere. Each returns 0 ok, 1 fail. */
static int
sandbox_before_create(void)
{
#ifdef HAVE_UNVEIL
	if (unveil("clp", "r") == -1) { perror("unveil clp"); return 1; }
	if (unveil(NULL, NULL) == -1) { perror("unveil seal"); return 1; }
#endif
#ifdef HAVE_PLEDGE
	if (pledge("stdio rpath", NULL) == -1) { perror("pledge"); return 1; }
#endif
	return 0;
}

static int
sandbox_after_create(void)
{
#ifdef HAVE_PLEDGE
	if (pledge("stdio", NULL) == -1) {
		perror("pledge drop rpath");
		return 1;
	}
#endif
	return 0;
}

int
main(void)
{
	if (sandbox_before_create() != 0)
		return 1;

	parse_env_budgets();

	fd_detector *fd = fd_create("clp");
	if (fd == NULL) {
		fputs("fd_create failed\n", stderr);
		return 1;
	}
	if (sandbox_after_create() != 0) {
		fd_destroy(fd);
		return 1;
	}

	printf("perf: budgets -- assert %" PRId64 " ns (%.2f ms), "
	    "query %" PRId64 " ns (%.2f ms)\n",
	    g_assert_budget, (double)g_assert_budget / 1e6,
	    g_query_budget,  (double)g_query_budget  / 1e6);

	run_warmup(fd);

	if (g_profile)
		fd_profile_begin(fd);

	/* Scenario B runs last on purpose: it asserts a navigation graph
	 * for vent detection, and that graph persists across fd_reset.
	 * If it ran before C, C's dense Δt=3 sightings (the synthetic
	 * stress shape) would trip false vents against B's graph. */
	run_scenario_a(fd);
	run_scenario_c(fd);
	run_scenario_b(fd);

	if (g_profile)
		fd_profile_end(fd);

	print_summary();

	if (g_profile) {
		fputs("\n--- CLIPS (profile-info) ---\n", stdout);
		fflush(stdout);
		fd_profile_dump(fd);
		fputs("--- end (profile-info) ---\n", stdout);
	}

	if (g_failures > 0) {
		printf("\nFAIL: %d call(s) exceeded budget\n", g_failures);
		fd_destroy(fd);
		return 1;
	}
	printf("\nPASS\n");
	fd_destroy(fd);
	return 0;
}
