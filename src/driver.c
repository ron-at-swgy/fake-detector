/*
 * driver.c - dev harness for libfakedetector.
 *
 * Drives a hand-coded ~200-tick scenario through the library, prints
 * the vote-time stance summary, and self-asserts API contracts. Exits
 * non-zero on any expected-value mismatch.
 *
 * Scenario (self = red, crewmate):
 *   blue   - three task completions, sighted in admin (alibied)  -> on-task
 *   lime   - sighted in cafeteria during kill window (alibied)   -> off-task
 *   pink   - sighted in storage near body, no alibi              -> threat
 *   green  - body discovered in storage at tick 178              -> dead
 *   yellow - impossible move medbay -> admin (vent)              -> threat
 *
 * main() is a sequence of phase helpers, each returning a failure
 * count; the harness exits non-zero if any helper reports a mismatch.
 */

#include <stdio.h>
#include <string.h>

#include "fakedetector.h"

#if defined(HAVE_PLEDGE) || defined(HAVE_UNVEIL)
#  include <unistd.h>
#endif

/* The synthetic round was authored against the legacy Among Them color
 * roster; registering the same 16 names keeps the golden snapshot
 * (tests/expected/driver.out) stable. The library itself is
 * identity-agnostic — this mapping is the harness's, not the API's. */
enum {
	P_RED = 0, P_ORANGE, P_YELLOW, P_LIGHT_BLUE,
	P_PINK, P_LIME, P_BLUE, P_PALE_BLUE,
	P_GRAY, P_WHITE, P_DARK_BROWN, P_BROWN,
	P_DARK_TEAL, P_GREEN, P_DARK_NAVY, P_BLACK,
	P__COUNT
};

static const char *const PLAYER_NAMES[P__COUNT] = {
	"red", "orange", "yellow", "light_blue",
	"pink", "lime", "blue", "pale_blue",
	"gray", "white", "dark_brown", "brown",
	"dark_teal", "green", "dark_navy", "black"
};

static void
register_players(fd_detector *fd)
{
	for (int i = 0; i < P__COUNT; i++)
		fd_set_player_name(fd, i, PLAYER_NAMES[i]);
}

static FILE *g_trace_capture;

/* ------------------------------------------------------------------
 * Expectation helpers -- each returns 0 on match, 1 on mismatch.
 * ------------------------------------------------------------------ */

static int
expect_stance(fd_detector *fd, fd_player who, fd_stance want, const char *label)
{
	fd_stance got = fd_get_stance(fd, who);
	if (got == want)
		return 0;
	fprintf(stderr, "FAIL stance %s: expected %d, got %d\n",
	    label, (int)want, (int)got);
	return 1;
}

static int
expect_evidence(fd_detector *fd, fd_player who, fd_evidence want,
    const char *label)
{
	fd_evidence got = fd_get_evidence(fd, who);
	if (got == want)
		return 0;
	fprintf(stderr, "FAIL evidence %s: expected %d, got %d\n",
	    label, (int)want, (int)got);
	return 1;
}

static int
expect_status(fd_detector *fd, fd_player who, fd_player_status want,
    const char *label)
{
	fd_player_status got = fd_get_status(fd, who);
	if (got == want)
		return 0;
	fprintf(stderr, "FAIL status %s: expected %d, got %d\n",
	    label, (int)want, (int)got);
	return 1;
}

static int
expect_risk(fd_detector *fd, fd_player who, fd_risk want, const char *label)
{
	fd_risk got = fd_alone_risk(fd, who);
	if (got == want)
		return 0;
	fprintf(stderr, "FAIL risk %s: expected %d, got %d\n",
	    label, (int)want, (int)got);
	return 1;
}

static int
expect_vote_pick(fd_detector *fd, fd_player want_color, fd_stance want_sev)
{
	fd_player got_color;
	fd_stance got_sev;
	int rc = fd_pick_vote_target(fd, &got_color, &got_sev);
	if (rc != 1) {
		fprintf(stderr, "FAIL fd_pick_vote_target: rc=%d "
		    "(expected 1)\n", rc);
		return 1;
	}
	if (got_color != want_color || got_sev != want_sev) {
		fprintf(stderr, "FAIL fd_pick_vote_target: got (%d, %d), "
		    "expected (%d, %d)\n",
		    (int)got_color, (int)got_sev,
		    (int)want_color, (int)want_sev);
		return 1;
	}
	return 0;
}

static const char *
pressure_word(fd_game_pressure_t p)
{
	switch (p) {
	case FD_PRESSURE_CRITICAL: return "critical";
	case FD_PRESSURE_HIGH:     return "high";
	case FD_PRESSURE_MEDIUM:   return "medium";
	case FD_PRESSURE_LOW:
	default:                   return "low";
	}
}

static const char *
vote_word(fd_vote_action a)
{
	switch (a) {
	case FD_VOTE_CAST:    return "CAST";
	case FD_VOTE_SKIP:    return "SKIP";
	case FD_VOTE_ABSTAIN:
	default:              return "ABSTAIN";
	}
}

/* Print the belief weight of every color that currently carries a
 * suspicion fact (alive non-self colors), in fd_player enum order. */
static void
print_suspicion(fd_detector *fd, const char *when)
{
	printf("  suspicion (%s):", when);
	for (int c = 0; c < P__COUNT; c++) {
		int w = fd_get_suspicion(fd, (fd_player)c);
		if (w >= 0)
			printf(" %s=%d", fd_player_name(fd, (fd_player)c), w);
	}
	printf("\n");
}

static int
expect_vote_decision(fd_detector *fd, fd_vote_action want, const char *label)
{
	fd_vote_decision_t d;
	int rc = fd_vote_decision(fd, &d);
	if (rc != 1) {
		fprintf(stderr, "FAIL vote_decision %s: rc=%d\n", label, rc);
		return 1;
	}
	printf("  vote decision (%s): %s target=%s suspicion=%d "
	    "confidence=%d\n    rationale: %s\n",
	    label, vote_word(d.recommendation), fd_player_name(fd, d.target),
	    d.suspicion, d.confidence, d.rationale);
	if (d.recommendation != want) {
		fprintf(stderr, "FAIL vote_decision %s: got %d, expected %d\n",
		    label, (int)d.recommendation, (int)want);
		return 1;
	}
	return 0;
}

/* Assert `higher`'s belief weight strictly exceeds `lower`'s. */
static int
expect_suspicion_order(fd_detector *fd, fd_player higher, fd_player lower,
    const char *label)
{
	int h = fd_get_suspicion(fd, higher);
	int l = fd_get_suspicion(fd, lower);
	if (h > l)
		return 0;
	fprintf(stderr, "FAIL suspicion order %s: %s=%d not above %s=%d\n",
	    label, fd_player_name(fd, higher), h, fd_player_name(fd, lower), l);
	return 1;
}

/* ------------------------------------------------------------------
 * Scenario setup
 * ------------------------------------------------------------------ */

/* Room navigation graph. Costs are in ticks (~25 ticks/sec). Adjacent
 * walks are 40-50 ticks; medbay<->admin is intentionally long (100) --
 * the gate YELLOW's vent trips against. Every normal walking move in
 * the scenario clears its link; only YELLOW's medbay->admin in 25
 * ticks (shortest path 90 via cafeteria) trips a vent. */
static void
setup_graph(fd_detector *fd)
{
	fd_add_room_link(fd, "cafeteria", "admin",   50);
	fd_add_room_link(fd, "cafeteria", "medbay",  40);
	fd_add_room_link(fd, "medbay",    "storage", 50);
	fd_add_room_link(fd, "medbay",    "admin",  100);
}

static void
feed_observations(fd_detector *fd)
{
	fd_observe_self(fd, 0, FD_PHASE_PREGAME, "cafeteria", 64, 64, 0);

	/* Round opens, all four others in cafeteria. */
	fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 64, 64, 0);
	fd_observe_player(fd, 10, P_BLUE,   "cafeteria", 70, 60);
	fd_observe_player(fd, 10, P_GREEN,  "cafeteria", 50, 70);
	fd_observe_player(fd, 10, P_PINK,   "cafeteria", 80, 80);
	fd_observe_player(fd, 10, P_LIME,   "cafeteria", 90, 65);
	fd_observe_player(fd, 10, P_YELLOW, "cafeteria", 95, 70);

	/* BLUE works tasks in cafeteria (t=30, 60) and admin (t=120); the
	 * t=150 admin sighting is inside green's kill window -> alibi. */
	fd_observe_player(fd, 30, P_BLUE,  "cafeteria", 72, 60);
	fd_observe_task_completion(fd, 30, P_BLUE, "cafeteria");
	fd_observe_player(fd, 60, P_BLUE,  "cafeteria", 75, 62);
	fd_observe_task_completion(fd, 60, P_BLUE, "cafeteria");
	fd_observe_player(fd, 120, P_BLUE, "admin", 100, 40);
	fd_observe_task_completion(fd, 120, P_BLUE, "admin");
	fd_observe_player(fd, 150, P_BLUE, "admin", 102, 42);

	/* GREEN drifts through medbay and storage. */
	fd_observe_player(fd, 80,  P_GREEN, "medbay",  20, 90);
	fd_observe_player(fd, 140, P_GREEN, "storage", 30, 110);

	/* PINK heads to storage at t=170 (the suspect-window sighting). */
	fd_observe_player(fd, 40,  P_PINK, "cafeteria", 78, 80);
	fd_observe_player(fd, 100, P_PINK, "medbay",    22, 88);
	fd_observe_player(fd, 170, P_PINK, "storage",   34, 112);

	/* LIME paces between rooms; the t=175 cafeteria sighting is within
	 * green's kill window -> alibi. */
	fd_observe_player(fd, 50,  P_LIME, "cafeteria", 88, 64);
	fd_observe_player(fd, 100, P_LIME, "medbay",    24, 92);
	fd_observe_player(fd, 175, P_LIME, "cafeteria", 86, 68);

	/* YELLOW: impossible movement. medbay t=100 -> admin t=125 = 25
	 * ticks; shortest walking path is 90 -> vent-suspected -> threat. */
	fd_observe_player(fd, 100, P_YELLOW, "medbay", 22, 88);
	fd_observe_player(fd, 125, P_YELLOW, "admin",  100, 40);

	/* Body discovered in storage at t=178; derives green=dead. */
	fd_observe_self(fd, 178, FD_PHASE_PLAYING, "storage", 28, 108, 0);
	fd_observe_body(fd, 178, P_GREEN, "storage", 35, 113);

	/* Voting opens. */
	fd_observe_self(fd, 200, FD_PHASE_VOTING, "cafeteria", 64, 64, 0);
}

/* ------------------------------------------------------------------
 * Output rendering + contract checks
 * ------------------------------------------------------------------ */

static int
render_outputs(fd_detector *fd)
{
	char buf[2048];
	if (fd_render_vote_summary(fd, buf, sizeof buf) < 0) {
		fputs("fd_render_vote_summary failed\n", stderr);
		return 1;
	}
	fputs(buf, stdout);
	if (fd_render_case(fd, buf, sizeof buf) < 0) {
		fputs("fd_render_case failed\n", stderr);
		return 1;
	}
	fputs(buf, stdout);
	return 0;
}

static int
check_stances(fd_detector *fd)
{
	int fails = 0;
	fails += expect_stance(fd, P_BLUE,   FD_STANCE_ON_TASK,  "blue");
	fails += expect_stance(fd, P_LIME,   FD_STANCE_OFF_TASK, "lime");
	fails += expect_stance(fd, P_PINK,   FD_STANCE_THREAT,   "pink");
	fails += expect_stance(fd, P_YELLOW, FD_STANCE_THREAT,   "yellow vent");
	/* dead color has no stance fact -> fd_get_stance returns UNKNOWN. */
	fails += expect_stance(fd, P_GREEN,  FD_STANCE_UNKNOWN,  "green");
	return fails;
}

/* The evidence kind behind each stance -- the threat-source signal a
 * policy thresholds its votes on. */
static int
check_evidence(fd_detector *fd)
{
	int fails = 0;
	fails += expect_evidence(fd, P_BLUE,   FD_EVIDENCE_ON_TASK,
	    "blue");
	fails += expect_evidence(fd, P_LIME,   FD_EVIDENCE_OFF_TASK,
	    "lime");
	fails += expect_evidence(fd, P_PINK,   FD_EVIDENCE_NEAR_BODY,
	    "pink near body");
	fails += expect_evidence(fd, P_YELLOW, FD_EVIDENCE_VENT,
	    "yellow vent");
	/* dead color has no stance fact -> fd_get_evidence returns NONE. */
	fails += expect_evidence(fd, P_GREEN,  FD_EVIDENCE_NONE,
	    "green");
	return fails;
}

/* fd_room_distance against the scenario graph from setup_graph:
 *   cafeteria-admin 50, cafeteria-medbay 40, medbay-storage 50,
 *   medbay-admin 100. A read-only query; no round state involved. */
static int
check_room_distance(fd_detector *fd)
{
	int fails = 0;
	struct { const char *a, *b; int want; } cases[] = {
		{ "cafeteria", "admin",   50 },  /* direct edge            */
		{ "medbay",    "admin",   90 },  /* via cafeteria, not 100 */
		{ "cafeteria", "storage", 90 },  /* two hops via medbay    */
		{ "admin",     "admin",    0 },  /* same known room        */
		{ "admin",     "lavatory", -1 }, /* unknown room           */
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		int got = fd_room_distance(fd, cases[i].a, cases[i].b);
		if (got != cases[i].want) {
			fprintf(stderr, "FAIL room_distance %s->%s: expected %d, "
			    "got %d\n", cases[i].a, cases[i].b, cases[i].want, got);
			fails++;
		}
	}
	if (fd_room_distance(fd, NULL, "admin") != -1 ||
	    fd_room_distance(fd, "admin", NULL) != -1) {
		fputs("FAIL room_distance NULL-arg contract\n", stderr);
		fails++;
	}
	return fails;
}

static int
check_statuses(fd_detector *fd)
{
	int fails = 0;
	fails += expect_status(fd, P_BLUE,   FD_PLAYER_STATUS_ALIVE, "blue");
	fails += expect_status(fd, P_LIME,   FD_PLAYER_STATUS_ALIVE, "lime");
	fails += expect_status(fd, P_PINK,   FD_PLAYER_STATUS_ALIVE, "pink");
	fails += expect_status(fd, P_YELLOW, FD_PLAYER_STATUS_ALIVE, "yellow");
	fails += expect_status(fd, P_GREEN,  FD_PLAYER_STATUS_DEAD,  "green");
	return fails;
}

static int
check_risks(fd_detector *fd)
{
	int fails = 0;
	fails += expect_risk(fd, P_BLUE,   FD_RISK_LOW,    "blue");
	fails += expect_risk(fd, P_LIME,   FD_RISK_MEDIUM, "lime");
	fails += expect_risk(fd, P_PINK,   FD_RISK_HIGH,   "pink");
	fails += expect_risk(fd, P_YELLOW, FD_RISK_HIGH,   "yellow vent");
	fails += expect_risk(fd, P_GREEN,  FD_RISK_NONE,   "green dead");
	fails += expect_risk(fd, P_RED,    FD_RISK_NONE,   "self red");
	fails += expect_risk(fd, P_ORANGE, FD_RISK_UNKNOWN,"orange unseen");

	/* Show the prose for the most informative pre-ejection cases. */
	char rbuf[256];
	if (fd_alone_risk_rationale(fd, P_PINK,   rbuf, sizeof rbuf) > 0)
		printf("  %s\n", rbuf);
	if (fd_alone_risk_rationale(fd, P_YELLOW, rbuf, sizeof rbuf) > 0)
		printf("  %s\n", rbuf);
	return fails;
}

/* ------------------------------------------------------------------
 * Playstyle demo: same evidence, three policy stances.
 * ------------------------------------------------------------------ */

static int
demo_playstyle(fd_detector *fd)
{
	int fails = 0;
	char rbuf[256];
	printf("\nplaystyle demo (same evidence, different policy stance):\n");

	fd_set_playstyle(fd, FD_PLAYSTYLE_TRUSTING);
	fails += expect_risk(fd, P_YELLOW, FD_RISK_HIGH, "trusting yellow");
	fails += expect_risk(fd, P_LIME,   FD_RISK_LOW,  "trusting lime");
	fails += expect_risk(fd, P_BLUE,   FD_RISK_LOW,  "trusting blue");
	if (fd_alone_risk_rationale(fd, P_LIME, rbuf, sizeof rbuf) > 0)
		printf("  %s\n", rbuf);

	fd_set_playstyle(fd, FD_PLAYSTYLE_PARANOID);
	fails += expect_risk(fd, P_YELLOW, FD_RISK_HIGH,   "paranoid yellow");
	fails += expect_risk(fd, P_LIME,   FD_RISK_HIGH,   "paranoid lime");
	fails += expect_risk(fd, P_BLUE,   FD_RISK_MEDIUM, "paranoid blue");
	if (fd_alone_risk_rationale(fd, P_BLUE, rbuf, sizeof rbuf) > 0)
		printf("  %s\n", rbuf);

	fd_set_playstyle(fd, FD_PLAYSTYLE_TRUSTING);
	fails += expect_vote_pick(fd, P_YELLOW, FD_STANCE_THREAT);

	fd_set_playstyle(fd, FD_PLAYSTYLE_NEUTRAL);
	return fails;
}

/* ------------------------------------------------------------------
 * Mid-round query demo: positional, ranking, round stats.
 * ------------------------------------------------------------------ */

static void
print_occupants(fd_detector *fd, const char *room)
{
	fd_player occ[P__COUNT];
	int n = fd_room_occupants(fd, room, occ, P__COUNT);
	printf("  occupants of %s:", room);
	for (int i = 0; i < n; i++)
		printf(" %s", fd_player_name(fd, occ[i]));
	printf("\n");
}

static int
demo_occupants(fd_detector *fd)
{
	print_occupants(fd, "cafeteria");
	print_occupants(fd, "admin");
	print_occupants(fd, "storage");

	fd_player occ[P__COUNT];
	int n = fd_room_occupants(fd, "storage", occ, P__COUNT);
	if (!(n == 1 && occ[0] == P_PINK)) {
		fprintf(stderr, "FAIL room_occupants storage: n=%d first=%d "
		    "(expected 1, pink)\n", n, n > 0 ? (int)occ[0] : -1);
		return 1;
	}
	/* Boundary cases. A NULL room is not a usage error -- no room
	 * named, no occupants -> 0. NULL out array or max <= 0 are caller
	 * mistakes -> -1. */
	if (fd_room_occupants(fd, NULL, occ, P__COUNT) != 0) {
		fprintf(stderr, "FAIL room_occupants NULL room\n");
		return 1;
	}
	if (fd_room_occupants(fd, "cafeteria", occ, 0) != -1) {
		fprintf(stderr, "FAIL room_occupants max=0\n");
		return 1;
	}
	if (fd_room_occupants(fd, "cafeteria", NULL, P__COUNT) != -1) {
		fprintf(stderr, "FAIL room_occupants NULL out array\n");
		return 1;
	}
	return 0;
}

static int
demo_last_seen(fd_detector *fd)
{
	int fails = 0;
	char where[32];
	int  lst;

	int r = fd_last_seen(fd, P_YELLOW, where, sizeof where, &lst);
	if (r == 1)
		printf("  last seen yellow: %s at tick %d\n", where, lst);
	if (r != 1 || strcmp(where, "admin") != 0 || lst != 125) {
		fprintf(stderr, "FAIL last_seen yellow: r=%d room=%s tick=%d "
		    "(expected 1, admin, 125)\n", r, where, lst);
		fails++;
	}

	r = fd_last_seen(fd, P_ORANGE, where, sizeof where, &lst);
	printf("  last seen orange: %s\n",
	    r == 0 ? "(never sighted)" : where);
	if (r != 0) {
		fprintf(stderr, "FAIL last_seen orange: r=%d (expected 0)\n", r);
		fails++;
	}
	return fails;
}

static int
demo_rank_suspects(fd_detector *fd)
{
	fd_player  rc[8];
	fd_stance rs[8];
	int n = fd_rank_suspects(fd, rc, rs, 8);
	printf("  top suspects (neutral):");
	for (int i = 0; i < n; i++) {
		const char *lbl =
		    rs[i] == FD_STANCE_THREAT   ? "threat"   :
		    rs[i] == FD_STANCE_OFF_TASK ? "off-task" :
		    rs[i] == FD_STANCE_ON_TASK  ? "on-task"  : "unknown";
		printf(" %s(%s)", fd_player_name(fd, rc[i]), lbl);
	}
	printf("\n");
	/* Expect YELLOW (THREAT), PINK (THREAT), LIME (OFF_TASK) leading. */
	if (n < 3 || rc[0] != P_YELLOW || rs[0] != FD_STANCE_THREAT ||
	    rc[1] != P_PINK   || rs[1] != FD_STANCE_THREAT ||
	    rc[2] != P_LIME   || rs[2] != FD_STANCE_OFF_TASK) {
		fprintf(stderr, "FAIL rank_suspects head: "
		    "[0]=%d/%d [1]=%d/%d [2]=%d/%d\n",
		    (int)rc[0], (int)rs[0], (int)rc[1], (int)rs[1],
		    (int)rc[2], (int)rs[2]);
		return 1;
	}
	/* Bad-args contract: NULL out array or max <= 0 -> -1. */
	if (fd_rank_suspects(fd, NULL, rs, 8) != -1 ||
	    fd_rank_suspects(fd, rc, rs, 0) != -1) {
		fprintf(stderr, "FAIL rank_suspects bad-args contract\n");
		return 1;
	}
	return 0;
}

static int
demo_round_stats(fd_detector *fd)
{
	fd_round_stats s;
	fd_round_stats_get(fd, &s);
	printf("  round stats: alive=%d dead=%d ejected=%d never-sighted=%d "
	    "open-cases=%d vents=%d impostor-found=%d\n",
	    s.alive, s.dead, s.ejected, s.never_sighted,
	    s.open_cases, s.vent_suspicions, s.impostor_found);
	if (s.alive != 4 || s.dead != 1 || s.ejected != 0 ||
	    s.open_cases != 1 || s.vent_suspicions != 1 ||
	    s.impostor_found != 0) {
		fprintf(stderr, "FAIL round_stats: alive=%d dead=%d ejected=%d "
		    "open_cases=%d vents=%d impostor=%d\n",
		    s.alive, s.dead, s.ejected, s.open_cases,
		    s.vent_suspicions, s.impostor_found);
		return 1;
	}
	return 0;
}

static int
demo_mid_round(fd_detector *fd)
{
	int fails = 0;
	printf("\nmid-round queries:\n");
	fails += demo_occupants(fd);
	fails += demo_last_seen(fd);
	fails += demo_rank_suspects(fd);
	fails += demo_round_stats(fd);
	return fails;
}

/* ------------------------------------------------------------------
 * Telemetry demo: suspicion attribution, run instrumentation, and a
 * structured working-memory dump -- the observability surface a policy
 * author tunes against.
 * ------------------------------------------------------------------ */

static int
demo_telemetry(fd_detector *fd)
{
	int fails = 0;
	printf("\ntelemetry:\n");

	/* Attribution: decompose yellow's suspicion weight into its
	 * evidence terms, then cross-check the breakdown. */
	fd_suspicion_explain ex;
	if (fd_explain_suspicion(fd, P_YELLOW, &ex) != 1) {
		fputs("FAIL explain_suspicion yellow: expected rc 1\n", stderr);
		return 1;
	}
	printf("  explain yellow: weight=%d logodds=%lld likelihood=%d "
	    "n_terms=%d\n", ex.weight, ex.logodds_total, ex.likelihood,
	    ex.n_terms);
	long long term_sum = 0;
	for (int i = 0; i < ex.n_terms; i++) {
		printf("    term: source=%s key='%s' amount=%lld\n",
		    ex.terms[i].source, ex.terms[i].key, ex.terms[i].amount);
		term_sum += ex.terms[i].amount;
	}
	if (term_sum != ex.logodds_total) {
		fprintf(stderr, "FAIL explain_suspicion: terms sum %lld != "
		    "logodds_total %lld\n", term_sum, ex.logodds_total);
		fails++;
	}
	if (ex.weight != fd_get_suspicion(fd, P_YELLOW)) {
		fputs("FAIL explain_suspicion: weight disagrees with "
		    "fd_get_suspicion\n", stderr);
		fails++;
	}
	/* A color with no suspicion fact (dead green) -> rc 0, no terms. */
	if (fd_explain_suspicion(fd, P_GREEN, &ex) != 0 ||
	    ex.n_terms != 0) {
		fputs("FAIL explain_suspicion green: expected rc 0, n_terms 0\n",
		    stderr);
		fails++;
	}
	if (fd_explain_suspicion(fd, P_YELLOW, NULL) != -1) {
		fputs("FAIL explain_suspicion NULL-out contract\n", stderr);
		fails++;
	}

	/* Run instrumentation. last_run_ms / max_run_ms are environment-
	 * sensitive, so they are checked but not printed. */
	fd_run_stats rs;
	fd_run_stats_get(fd, &rs);
	printf("  run stats: runs=%lld last-fired=%lld total-fired=%lld "
	    "facts=%d\n", rs.run_count, rs.last_rules_fired,
	    rs.total_rules_fired, rs.fact_count);
	if (rs.run_count <= 0 || rs.total_rules_fired <= 0 ||
	    rs.fact_count <= 0 || rs.last_run_ms < 0.0 ||
	    rs.max_run_ms < rs.last_run_ms) {
		fprintf(stderr, "FAIL run_stats invariants: runs=%lld "
		    "total=%lld facts=%d last_ms=%.3f max_ms=%.3f\n",
		    rs.run_count, rs.total_rules_fired, rs.fact_count,
		    rs.last_run_ms, rs.max_run_ms);
		fails++;
	}

	/* Structured working-memory snapshot. */
	fd_dump_state(fd, stdout);
	return fails;
}

/* ------------------------------------------------------------------
 * Post-ejection and post-reset checks
 * ------------------------------------------------------------------ */

static int
check_post_ejection(fd_detector *fd)
{
	int fails = 0;
	fd_observe_ejection(fd, 250, P_PINK, /*was_impostor=*/1);
	fails += expect_status(fd, P_PINK, FD_PLAYER_STATUS_EJECTED,
	    "pink after ejection");

	/* PINK ejected; YELLOW's threat is demoted by impostor-found, then
	 * stance-off-task fires -> YELLOW wins the off-task tie-break. */
	fd_player  next_color;
	fd_stance next_sev;
	int rc = fd_pick_vote_target(fd, &next_color, &next_sev);
	if (rc != 1 || next_color != P_YELLOW ||
	    next_sev != FD_STANCE_OFF_TASK) {
		fprintf(stderr, "FAIL post-ejection vote-pick: rc=%d color=%d "
		    "sev=%d (expected 1, yellow, off-task)\n",
		    rc, (int)next_color, (int)next_sev);
		fails++;
	}
	/* YELLOW's vent threat was demoted to none by impostor-found, then
	 * re-promoted by stance-off-task -- evidence tracks the new verdict. */
	fails += expect_evidence(fd, P_YELLOW, FD_EVIDENCE_OFF_TASK,
	    "yellow post-eject");

	/* impostor-found short-circuits all alive crewmates to NONE. */
	fails += expect_risk(fd, P_LIME, FD_RISK_NONE, "lime post-eject");
	fails += expect_risk(fd, P_BLUE, FD_RISK_NONE, "blue post-eject");
	char rbuf[256];
	if (fd_alone_risk_rationale(fd, P_LIME, rbuf, sizeof rbuf) > 0)
		printf("  %s\n", rbuf);

	/* TRUSTING abstains: no threats remain, and it won't vote off-task. */
	fd_set_playstyle(fd, FD_PLAYSTYLE_TRUSTING);
	fd_player tc;
	fd_stance ts;
	if (fd_pick_vote_target(fd, &tc, &ts) != 0) {
		fprintf(stderr, "FAIL trusting post-eject pick: expected 0, "
		    "got color=%d\n", (int)tc);
		fails++;
	}
	/* Stay PARANOID across the upcoming reset to verify persistence. */
	fd_set_playstyle(fd, FD_PLAYSTYLE_PARANOID);
	return fails;
}

static int
check_post_reset(fd_detector *fd)
{
	int fails = 0;
	fd_reset(fd);
	if (fd_get_playstyle(fd) != FD_PLAYSTYLE_PARANOID) {
		fprintf(stderr,
		    "FAIL playstyle did not persist across fd_reset\n");
		fails++;
	}
	/* Restore NEUTRAL so the post-reset risk/vote checks see legacy
	 * behavior. */
	fd_set_playstyle(fd, FD_PLAYSTYLE_NEUTRAL);

	fails += expect_status(fd, P_PINK,  FD_PLAYER_STATUS_UNKNOWN,
	    "pink after reset");
	fails += expect_status(fd, P_GREEN, FD_PLAYER_STATUS_UNKNOWN,
	    "green after reset");
	fails += expect_status(fd, P_BLUE,  FD_PLAYER_STATUS_UNKNOWN,
	    "blue after reset");
	fails += expect_risk(fd, P_PINK, FD_RISK_UNKNOWN,
	    "pink after reset");

	fd_player  next_color;
	fd_stance next_sev;
	if (fd_pick_vote_target(fd, &next_color, &next_sev) != 0) {
		fprintf(stderr, "FAIL post-reset vote-pick: expected 0\n");
		fails++;
	}

	/* PARANOID will vote even an alive color with UNKNOWN stance. */
	fd_set_self(fd, P_RED);
	fd_observe_player(fd, 10, P_ORANGE, "cafeteria", 60, 60);
	fd_set_playstyle(fd, FD_PLAYSTYLE_PARANOID);
	fd_player tc;
	fd_stance ts;
	int trc = fd_pick_vote_target(fd, &tc, &ts);
	if (trc != 1 || tc != P_ORANGE) {
		fprintf(stderr, "FAIL paranoid unknown-vote: rc=%d color=%d "
		    "(expected 1, orange)\n", trc, (int)tc);
		fails++;
	}

	/* Room-name validation: an observation naming a room with
	 * non-symbol characters is rejected outright -- no sighting fact
	 * is asserted, so WHITE's status stays UNKNOWN. Guards against
	 * CLIPS fact-string injection via untrusted room names. */
	fd_observe_player(fd, 12, P_WHITE, "bad room!", 0, 0);
	fails += expect_status(fd, P_WHITE, FD_PLAYER_STATUS_UNKNOWN,
	    "white bad-room rejected");
	return fails;
}

/* ------------------------------------------------------------------
 * Detective-reasoning scenarios -- each runs on its own fd_reset round
 * and exercises one finding of the cast-iron-alibi redesign.
 * ------------------------------------------------------------------ */

/* Item 1 -- an alibi must be reconsidered when the kill window
 * narrows. brown was glimpsed once, in admin -- 140 ticks from the
 * storage murder scene. While green's window is the whole round
 * [0,178] that is a cast-iron alibi and brown is cleared (no suspect
 * at all). A later-processed sighting of green alive in storage at
 * tick 130 ratchets the window to [130,178]; brown's tick-100
 * sighting now falls outside it entirely. The `logical` CE on
 * derive-alibi retracts the stale alibi, the cast-iron alibi falls
 * with it, brown becomes the lone suspect and the case is solved
 * against them -- off-task flips to threat. Without truth maintenance
 * brown would stay wrongly cleared. */
static int
scenario_stale_alibi(fd_detector *fd)
{
	int fails = 0;
	char buf[2048];
	printf("\nscenario: stale alibi (kill window narrows after the "
	    "alibi was granted)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);

	/* Phase A: green was never sighted alive, so the kill window is
	 * the whole round [0,178]. brown's lone admin sighting clears
	 * them cast-iron. */
	fd_observe_player(fd, 100, P_BROWN, "admin", 100, 40);
	fd_observe_body(fd, 178, P_GREEN, "storage", 35, 113);

	if (fd_render_case(fd, buf, sizeof buf) >= 0)
		fputs(buf, stdout);
	fails += expect_stance(fd, P_BROWN, FD_STANCE_OFF_TASK,
	    "brown cast-iron cleared (wide window)");

	/* Phase B: a late-processed sighting places green alive in
	 * storage at tick 130 -- the window narrows to [130,178] and
	 * brown's tick-100 alibi no longer falls inside it. */
	fd_observe_player(fd, 130, P_GREEN, "storage", 30, 110);

	if (fd_render_case(fd, buf, sizeof buf) >= 0)
		fputs(buf, stdout);
	fails += expect_stance(fd, P_BROWN, FD_STANCE_THREAT,
	    "brown re-suspected, case solved (window narrowed)");
	return fails;
}

/* Item 2 -- a single glimpse is not an alibi. orange and white were
 * each glimpsed exactly once during green's kill window. orange's
 * glimpse was in medbay, 50 ticks from the storage scene -- a round
 * trip fits the [0,178] window, so it is only a WEAK alibi and orange
 * stays a suspect (in fact the lone suspect, so the case is solved
 * against them). white's glimpse was in admin, 140 ticks away: the
 * murder was physically impossible, a cast-iron alibi, and white is
 * cleared outright. */
static int
scenario_weak_vs_cast_iron(fd_detector *fd)
{
	int fails = 0;
	char buf[2048];
	printf("\nscenario: weak vs cast-iron alibi (one glimpse does not "
	    "clear a suspect)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);

	/* green never sighted alive -> kill window [0,178]. */
	fd_observe_player(fd, 100, P_ORANGE, "medbay", 22, 88);
	fd_observe_player(fd,  90, P_WHITE,  "admin", 100, 40);
	fd_observe_body(fd, 178, P_GREEN, "storage", 35, 113);

	if (fd_render_case(fd, buf, sizeof buf) >= 0)
		fputs(buf, stdout);

	fails += expect_stance(fd, P_WHITE, FD_STANCE_OFF_TASK,
	    "white cast-iron alibi (cleared)");
	fails += expect_stance(fd, P_ORANGE, FD_STANCE_THREAT,
	    "orange weak alibi (not cleared -- lone suspect, accused)");
	/* The accused's threat rests on a solved case, not a vent or body. */
	fails += expect_evidence(fd, P_ORANGE, FD_EVIDENCE_ACCUSATION,
	    "orange accused");
	return fails;
}

/* Item 3 -- a directly observed vent. dark_teal is sighted in storage,
 * then witnessed using a vent there (fd_observe_vent). Unlike the
 * inferred routing-distance check this needs no room-graph reasoning:
 * the observation alone promotes dark_teal to a THREAT with evidence
 * VENT, and the rationale cites the sighting rather than "impossible
 * movement". */
static int
scenario_observed_vent(fd_detector *fd)
{
	int fails = 0;
	char buf[256];
	printf("\nscenario: directly observed vent (a witnessed vent use, "
	    "independent of the room graph)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);

	/* The sighting that saw the vent gives dark_teal an alive dossier;
	 * fd_observe_vent then asserts the confirmed vent. */
	fd_observe_player(fd, 60, P_DARK_TEAL, "storage", 30, 110);
	fd_observe_vent(fd, 64, P_DARK_TEAL, "storage");

	fails += expect_stance(fd, P_DARK_TEAL, FD_STANCE_THREAT,
	    "dark_teal observed venting");
	fails += expect_evidence(fd, P_DARK_TEAL, FD_EVIDENCE_VENT,
	    "dark_teal observed venting");

	if (fd_get_rationale(fd, P_DARK_TEAL, buf, sizeof buf) > 0) {
		printf("  rationale: %s\n", buf);
		if (strstr(buf, "seen using a vent") == NULL) {
			fprintf(stderr, "FAIL observed-vent rationale: %s\n", buf);
			fails++;
		}
	}
	return fails;
}

/* ------------------------------------------------------------------
 * Probabilistic-layer scenarios (Enh. 1 / 2 / 6).
 * ------------------------------------------------------------------ */

/* Enh. 2 -- a round with TWO impostors. The first impostor ejection
 * must NOT clear the board: the round-end demotion fires only once
 * every impostor is accounted for. yellow vents and pink is found near
 * a body, so both are threats; ejecting pink (impostor 1 of 2) leaves
 * yellow's threat standing, and only the second impostor ejection
 * demotes it. Also exercises fd_get_suspicion, fd_game_pressure and
 * fd_win_probability. */
static int
scenario_multi_impostor(fd_detector *fd)
{
	int fails = 0;
	printf("\nscenario: multi-impostor round (two impostors; the first "
	    "ejection must not clear the board)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 7, 2);

	/* The body is in admin. yellow vents into it (medbay t=100 ->
	 * admin t=125, shortest walking path 90) and pink is sighted at
	 * the scene; orange, blue and lime are sighted but idle. yellow's
	 * lone medbay sighting is too recent to be a cast-iron alibi, so
	 * the vent evidence stands. */
	fd_observe_player(fd, 60,  P_ORANGE, "cafeteria", 60, 60);
	fd_observe_player(fd, 60,  P_BLUE,   "cafeteria", 70, 60);
	fd_observe_player(fd, 60,  P_LIME,   "cafeteria", 90, 65);
	fd_observe_player(fd, 100, P_YELLOW, "medbay",    22, 88);
	fd_observe_player(fd, 125, P_YELLOW, "admin",    100, 40);
	fd_observe_player(fd, 150, P_PINK,   "admin",    104, 42);
	fd_observe_body(fd, 178, P_GREEN, "admin", 100, 45);

	fails += expect_stance(fd, P_YELLOW, FD_STANCE_THREAT,
	    "yellow vent threat");
	fails += expect_stance(fd, P_PINK, FD_STANCE_THREAT,
	    "pink near-body threat");

	printf("  game pressure: %s, crew win prob: %d permille\n",
	    pressure_word(fd_game_pressure(fd)), fd_win_probability(fd));
	print_suspicion(fd, "before any ejection");

	/* The probabilistic layer must rank a vent threat above an idle
	 * crewmate. */
	if (fd_get_suspicion(fd, P_YELLOW)
	    <= fd_get_suspicion(fd, P_LIME)) {
		fprintf(stderr, "FAIL suspicion: yellow not above lime\n");
		fails++;
	}

	/* Eject the FIRST impostor. With two configured, this must NOT
	 * clear yellow's threat (the Enh. 2 correctness fix). */
	fd_observe_ejection(fd, 250, P_PINK, /*was_impostor=*/1);
	fails += expect_stance(fd, P_YELLOW, FD_STANCE_THREAT,
	    "yellow still a threat after the first of two impostors");
	print_suspicion(fd, "after first impostor ejected");

	/* Eject the SECOND impostor. Every impostor is now accounted for,
	 * so the lingering threat is demoted. */
	fd_observe_ejection(fd, 260, P_ORANGE, /*was_impostor=*/1);
	if (fd_get_stance(fd, P_YELLOW) == FD_STANCE_THREAT) {
		fprintf(stderr, "FAIL yellow still a threat after BOTH "
		    "impostors ejected\n");
		fails++;
	}
	printf("  game pressure: %s, crew win prob: %d permille\n",
	    pressure_word(fd_game_pressure(fd)), fd_win_probability(fd));
	return fails;
}

/* Enh. 6 -- the expected-value vote gate. Two rounds: a flat one where
 * no candidate stands out (the decision must not be CAST), and an
 * endgame where a dominant vent suspect at critical pressure clears the
 * expected-value bar (CAST). */
static int
scenario_vote_decision(fd_detector *fd)
{
	int fails = 0;
	printf("\nscenario: expected-value vote gating\n");

	/* Flat round: four idle colors, nobody distinguishable. */
	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 6, 1);
	fd_observe_player(fd, 20, P_ORANGE, "cafeteria", 60, 60);
	fd_observe_player(fd, 20, P_PINK,   "cafeteria", 64, 62);
	fd_observe_player(fd, 20, P_BLUE,   "cafeteria", 68, 64);
	fd_observe_player(fd, 20, P_LIME,   "cafeteria", 72, 66);
	{
		fd_vote_decision_t d;
		fd_vote_decision(fd, &d);
		printf("  vote decision (flat round): %s target=%s "
		    "suspicion=%d confidence=%d\n    rationale: %s\n",
		    vote_word(d.recommendation), fd_player_name(fd, d.target),
		    d.suspicion, d.confidence, d.rationale);
		if (d.recommendation == FD_VOTE_CAST) {
			fprintf(stderr, "FAIL flat-round vote: should not CAST\n");
			fails++;
		}
	}

	/* Endgame: self plus two others, one caught venting. Critical
	 * pressure and a dominant suspect -> the EV test says CAST. */
	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 3, 1);
	fd_observe_player(fd, 40, P_YELLOW, "storage", 30, 110);
	fd_observe_vent(fd, 44, P_YELLOW, "storage");
	fd_observe_player(fd, 50, P_BLUE, "cafeteria", 70, 60);
	fd_observe_task_completion(fd, 52, P_BLUE, "cafeteria");
	fd_observe_task_completion(fd, 70, P_BLUE, "cafeteria");
	printf("  game pressure: %s, crew win prob: %d permille\n",
	    pressure_word(fd_game_pressure(fd)), fd_win_probability(fd));
	print_suspicion(fd, "endgame");
	fails += expect_vote_decision(fd, FD_VOTE_CAST, "endgame, vent caught");
	return fails;
}

/* Enh. 3 -- the testimony / claims layer. gray is an idle baseline.
 * pink claims it was in cafeteria at tick 100 but was sighted in
 * storage at that tick -- a geometric contradiction. yellow self-
 * reports the body. lime and blue both vouch for admin at overlapping
 * ticks -- a mutual weak alibi. brown was at the storage scene but
 * files no claim -- a silent witness. Every effect surfaces through
 * fd_get_suspicion. */
static int
scenario_claims(fd_detector *fd)
{
	int fails = 0;
	printf("\nscenario: testimony / claims layer (chat checked against "
	    "the record)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 8, 1);

	/* Dossiers for everyone. */
	fd_observe_player(fd, 10, P_PINK,   "cafeteria", 60, 60);
	fd_observe_player(fd, 10, P_YELLOW, "cafeteria", 64, 60);
	fd_observe_player(fd, 10, P_LIME,   "cafeteria", 68, 60);
	fd_observe_player(fd, 10, P_BLUE,   "cafeteria", 72, 60);
	fd_observe_player(fd, 10, P_GRAY,   "cafeteria", 76, 60);
	fd_observe_player(fd, 10, P_BROWN,  "cafeteria", 80, 60);

	/* pink is sighted in storage at tick 100; brown lingers at the
	 * storage scene; a body is found there. */
	fd_observe_player(fd, 100, P_PINK,  "storage", 30, 110);
	fd_observe_player(fd, 170, P_BROWN, "storage", 34, 112);
	fd_observe_body(fd, 180, P_GREEN, "storage", 35, 113);

	/* Vote-chat claims the policy parsed (tick = the tick claimed). */
	fd_observe_claim(fd, 100, P_PINK,   FD_CLAIM_LOCATION,
	    "cafeteria");                       /* refuted by the storage sighting */
	fd_observe_claim(fd, 150, P_YELLOW, FD_CLAIM_SELF_REPORT,
	    "storage");                         /* "I found the body" */
	fd_observe_claim(fd, 150, P_LIME,   FD_CLAIM_LOCATION, "admin");
	fd_observe_claim(fd, 160, P_BLUE,   FD_CLAIM_LOCATION, "admin");
	/* gray and brown make no claim. */

	print_suspicion(fd, "claims layer");

	fails += expect_suspicion_order(fd, P_PINK, P_GRAY,
	    "contradicted claim outranks an idle peer");
	fails += expect_suspicion_order(fd, P_YELLOW, P_GRAY,
	    "self-report outranks an idle peer");
	fails += expect_suspicion_order(fd, P_BROWN, P_GRAY,
	    "silent witness outranks an idle peer");
	fails += expect_suspicion_order(fd, P_GRAY, P_LIME,
	    "co-located pair clears below an idle peer (lime)");
	fails += expect_suspicion_order(fd, P_GRAY, P_BLUE,
	    "co-located pair clears below an idle peer (blue)");
	return fails;
}

/* Enh. 5 -- the accusation / defense social graph. pink is the round's
 * impostor. orange accused pink, blue defended pink, lime co-accused
 * gray alongside pink, and white piled onto brown (whom a cast-iron
 * alibi clears). The votes are inert until pink's ejection reveals the
 * impostor -- then the whole table is retro-scored. */
static int
scenario_social(fd_detector *fd)
{
	int fails = 0;
	printf("\nscenario: accusation / defense social graph "
	    "(one reveal re-reads the table)\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 9, 1);

	fd_observe_player(fd, 10, P_ORANGE, "cafeteria", 60, 60);
	fd_observe_player(fd, 10, P_BLUE,   "cafeteria", 64, 60);
	fd_observe_player(fd, 10, P_GRAY,   "cafeteria", 68, 60);
	fd_observe_player(fd, 10, P_LIME,   "cafeteria", 72, 60);
	fd_observe_player(fd, 10, P_WHITE,  "cafeteria", 76, 60);
	fd_observe_player(fd, 10, P_PINK,   "cafeteria", 80, 60);

	/* brown is sighted in admin -- far from the storage scene, so the
	 * case layer will clear brown with a cast-iron alibi. */
	fd_observe_player(fd, 100, P_BROWN, "admin", 100, 40);
	fd_observe_body(fd, 180, P_GREEN, "storage", 35, 113);

	/* The meeting: votes cast and a defense spoken. */
	fd_observe_vote(fd, 200, P_ORANGE, P_PINK);  /* accuses impostor */
	fd_observe_defense(fd, 200, P_BLUE, P_PINK); /* defends impostor */
	fd_observe_vote(fd, 200, P_PINK, P_GRAY);    /* impostor accuses gray */
	fd_observe_vote(fd, 200, P_LIME, P_GRAY);    /* lime co-accuses gray */
	fd_observe_vote(fd, 200, P_WHITE, P_BROWN);  /* white piles on the cleared */

	print_suspicion(fd, "before the reveal");

	/* pink is ejected and revealed an impostor -- retro-update fires. */
	fd_observe_ejection(fd, 210, P_PINK, /*was_impostor=*/1);
	print_suspicion(fd, "after pink revealed impostor");

	fails += expect_suspicion_order(fd, P_BLUE, P_ORANGE,
	    "defender of the impostor outranks an accuser of the impostor");
	fails += expect_suspicion_order(fd, P_WHITE, P_ORANGE,
	    "accusing a cleared color outranks accusing the impostor");
	fails += expect_suspicion_order(fd, P_LIME, P_ORANGE,
	    "co-accusing with the impostor outranks accusing it");
	if (fd_get_suspicion(fd, P_PINK) != -1) {
		fprintf(stderr, "FAIL social: ejected pink still has a "
		    "suspicion weight\n");
		fails++;
	}
	return fails;
}

/* Telemetry -- the capturable rule-firing trace. A fresh round with a
 * body feeds the dossier and case rules; running it inside
 * fd_trace_begin / fd_trace_end mirrors the rule / fact / activation
 * churn to a stream. The captured volume is CLIPS-version-coupled, so
 * the test asserts only that capture happened -- the bytes themselves
 * are not snapshotted. */
static int
scenario_trace(fd_detector *fd)
{
	printf("\nscenario: capturable rule-firing trace\n");

	fd_reset(fd);
	fd_set_self(fd, P_RED);
	fd_observe_player(fd, 60, P_PINK, "storage", 30, 110);
	fd_observe_body(fd, 120, P_GREEN, "storage", 35, 113);

	FILE *tf = g_trace_capture;
	if (tf == NULL) {
		fputs("FAIL trace: capture file unavailable\n", stderr);
		return 1;
	}
	fd_trace_begin(fd, tf);
	fd_run(fd);            /* fires the dossier / case rules under trace */
	fd_trace_end(fd);

	fflush(tf);
	long n = ftell(tf);
	rewind(tf);

	/* NULL-safety contract: neither call crashes on a NULL argument. */
	fd_trace_begin(fd, NULL);
	fd_trace_begin(NULL, NULL);
	fd_trace_end(NULL);

	printf("  trace captured %s\n",
	    n > 0 ? "rule-firing output" : "nothing");
	if (n <= 0) {
		fputs("FAIL trace: no output captured\n", stderr);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * Sandbox (OpenBSD pledge/unveil) -- no-ops elsewhere.
 * ------------------------------------------------------------------ */

static int
sandbox_before_create(void)
{
#ifdef HAVE_UNVEIL
	if (unveil("clp", "r") == -1) { perror("unveil clp"); return 1; }
	if (unveil("/tmp", "rwc") == -1) { perror("unveil /tmp"); return 1; }
	if (unveil(NULL, NULL) == -1) { perror("unveil seal"); return 1; }
#endif
#ifdef HAVE_PLEDGE
	if (pledge("stdio rpath wpath cpath", NULL) == -1) {
		perror("pledge");
		return 1;
	}
#endif
	g_trace_capture = tmpfile();
	if (g_trace_capture == NULL) {
		perror("tmpfile");
		return 1;
	}
#ifdef HAVE_PLEDGE
	if (pledge("stdio rpath", NULL) == -1) {
		perror("pledge drop write/create");
		fclose(g_trace_capture);
		g_trace_capture = NULL;
		return 1;
	}
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

	fd_detector *fd = fd_create("clp");
	if (fd == NULL) {
		fputs("fd_create failed (rule files missing or invalid?)\n",
		    stderr);
		fclose(g_trace_capture);
		g_trace_capture = NULL;
		return 1;
	}
	if (sandbox_after_create() != 0) {
		fd_destroy(fd);
		fclose(g_trace_capture);
		g_trace_capture = NULL;
		return 1;
	}

	register_players(fd);
	fd_set_self(fd, P_RED);
	setup_graph(fd);
	feed_observations(fd);

	int fails = 0;
	fails += render_outputs(fd);
	fails += check_stances(fd);
	fails += check_evidence(fd);
	fails += check_room_distance(fd);
	fails += check_statuses(fd);
	fails += check_risks(fd);
	fails += expect_vote_pick(fd, P_YELLOW, FD_STANCE_THREAT);
	fails += demo_playstyle(fd);
	fails += demo_mid_round(fd);
	fails += demo_telemetry(fd);
	fails += check_post_ejection(fd);
	fails += check_post_reset(fd);
	fails += scenario_stale_alibi(fd);
	fails += scenario_weak_vs_cast_iron(fd);
	fails += scenario_observed_vent(fd);
	fails += scenario_multi_impostor(fd);
	fails += scenario_vote_decision(fd);
	fails += scenario_claims(fd);
	fails += scenario_social(fd);
	fails += scenario_trace(fd);

	fd_destroy(fd);
	fclose(g_trace_capture);
	g_trace_capture = NULL;
	return fails == 0 ? 0 : 1;
}
