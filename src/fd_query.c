/*
 * fd_query.c -- read-only situational queries.
 *
 * The getters a policy calls to act: per-color stance/status/risk,
 * the vote-target picker and suspect ranking, and the positional and
 * round-aggregate queries. None mutate working memory beyond the
 * implicit fd_run() they share with every query entry point.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fd_internal.h"

fd_stance
fd_get_stance_now(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_STANCE_UNKNOWN;
	Fact *f = fd_find_stance(fd, who);
	if (f == NULL)
		return FD_STANCE_UNKNOWN;
	return fd_parse_category(fd_fact_symbol(f, "category"));
}

fd_evidence
fd_get_evidence_now(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_EVIDENCE_NONE;
	Fact *f = fd_find_stance(fd, who);
	if (f == NULL)
		return FD_EVIDENCE_NONE;
	return fd_parse_evidence(fd_fact_symbol(f, "evidence"));
}

fd_player_status
fd_get_status_now(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_PLAYER_STATUS_UNKNOWN;
	Fact *f = fd_find_crewmember(fd, who);
	if (f == NULL)
		return FD_PLAYER_STATUS_UNKNOWN;
	return fd_parse_status(fd_fact_symbol(f, "status"));
}

int
fd_get_suspicion_now(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return -1;
	Fact *s = fd_find_suspicion(fd, who);
	if (s == NULL)
		return -1;
	return (int)fd_fact_int(s, "weight");
}

fd_stance
fd_get_stance(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_STANCE_UNKNOWN;
	fd_run(fd);
	return fd_get_stance_now(fd, who);
}

fd_evidence
fd_get_evidence(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_EVIDENCE_NONE;
	fd_run(fd);
	return fd_get_evidence_now(fd, who);
}

fd_player_status
fd_get_status(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_PLAYER_STATUS_UNKNOWN;
	fd_run(fd);
	return fd_get_status_now(fd, who);
}

/* ------------------------------------------------------------------
 * Vote-target selection
 * ------------------------------------------------------------------ */

/* Per-playstyle vote-candidate priority. Shared by fd_pick_vote_target
 * (top-1) and fd_rank_suspects (top-N).
 *   TRUSTING — only a confirmed THREAT is worth a vote.
 *   NEUTRAL  — THREAT > OFF_TASK > ON_TASK; skip UNKNOWN.
 *   PARANOID — same but include UNKNOWN if nobody else qualifies. */
static const fd_stance PRIORITY_TRUSTING[] = {
	FD_STANCE_THREAT
};
static const fd_stance PRIORITY_NEUTRAL[] = {
	FD_STANCE_THREAT, FD_STANCE_OFF_TASK, FD_STANCE_ON_TASK
};
static const fd_stance PRIORITY_PARANOID[] = {
	FD_STANCE_THREAT, FD_STANCE_OFF_TASK, FD_STANCE_ON_TASK,
	FD_STANCE_UNKNOWN
};

static void
priority_for_style(fd_playstyle style, const fd_stance **out, size_t *out_n)
{
	switch (style) {
	case FD_PLAYSTYLE_TRUSTING:
		*out   = PRIORITY_TRUSTING;
		*out_n = sizeof PRIORITY_TRUSTING / sizeof PRIORITY_TRUSTING[0];
		return;
	case FD_PLAYSTYLE_PARANOID:
		*out   = PRIORITY_PARANOID;
		*out_n = sizeof PRIORITY_PARANOID / sizeof PRIORITY_PARANOID[0];
		return;
	case FD_PLAYSTYLE_NEUTRAL:
	default:
		*out   = PRIORITY_NEUTRAL;
		*out_n = sizeof PRIORITY_NEUTRAL / sizeof PRIORITY_NEUTRAL[0];
		return;
	}
}

/* True if `c` is an alive, non-self color whose stance matches `want`. */
static bool
candidate_matches(fd_detector *fd, int c, fd_stance want)
{
	if (fd->self_color != SELF_UNSET && (fd_player)c == fd->self_color)
		return false;
	if (fd_get_status_now(fd, (fd_player)c) != FD_PLAYER_STATUS_ALIVE)
		return false;
	return fd_get_stance_now(fd, (fd_player)c) == want;
}

int
fd_pick_vote_target(fd_detector *fd, fd_player *out_color,
    fd_stance *out_severity)
{
	if (fd == NULL || out_color == NULL || out_severity == NULL)
		return -1;
	fd_run(fd);

	const fd_stance *priority;
	size_t           n_priority;
	priority_for_style(fd->style, &priority, &n_priority);

	for (size_t i = 0; i < n_priority; i++) {
		for (int c = 0; c < FD_MAX_PLAYERS; c++) {
			if (!candidate_matches(fd, c, priority[i]))
				continue;
			*out_color    = (fd_player)c;
			*out_severity = priority[i];
			return 1;
		}
	}
	return 0;
}

/* Rank alive non-self colors by probabilistic belief weight, highest
 * first. Ties break by fd_player enum order (the gather loop visits
 * colors in enum order and the sort below is stable). This ranks by the
 * Enh. 1 suspicion distribution, NOT by stance tier -- so its head can
 * differ from fd_pick_vote_target's pick, which is still stance-tier +
 * playstyle based. fd_vote_decision is the belief-aware vote picker. */
int
fd_rank_suspects(fd_detector *fd, fd_player *out_colors,
    fd_stance *out_stances, int max)
{
	if (fd == NULL || out_colors == NULL || out_stances == NULL || max <= 0)
		return -1;
	fd_run(fd);

	fd_player colors[FD_MAX_PLAYERS];
	int      weights[FD_MAX_PLAYERS];
	int      n = 0;
	for (int c = 0; c < FD_MAX_PLAYERS; c++) {
		if (fd->self_color != SELF_UNSET && (fd_player)c == fd->self_color)
			continue;
		Fact *s = fd_find_suspicion(fd, (fd_player)c);
		if (s == NULL)   /* dead / ejected / never-sighted: no fact */
			continue;
		colors[n]  = (fd_player)c;
		weights[n] = (int)fd_fact_int(s, "weight");
		n++;
	}
	/* Insertion sort, weight descending. The strict `<` keeps equal
	 * weights in their original (enum) order -- a stable tie-break. */
	for (int i = 1; i < n; i++) {
		fd_player ck = colors[i];
		int      wk = weights[i];
		int      j  = i - 1;
		while (j >= 0 && weights[j] < wk) {
			colors[j + 1]  = colors[j];
			weights[j + 1] = weights[j];
			j--;
		}
		colors[j + 1]  = ck;
		weights[j + 1] = wk;
	}
	int written = (n < max) ? n : max;
	for (int i = 0; i < written; i++) {
		out_colors[i]  = colors[i];
		out_stances[i] = fd_get_stance_now(fd, colors[i]);
	}
	return written;
}

/* ------------------------------------------------------------------
 * Probabilistic belief queries (Enh. 1) and round model (Enh. 2)
 * ------------------------------------------------------------------ */

int
fd_get_suspicion(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return -1;
	fd_run(fd);
	return fd_get_suspicion_now(fd, who);
}

/* Telemetry: decompose a color's suspicion weight into the evidence
 * terms behind it. Reads the same (logodds-term) facts fd_run's
 * normalization pass sums -- this just keeps the per-term breakdown the
 * arithmetic pass discards. */
int
fd_explain_suspicion(fd_detector *fd, fd_player who, fd_suspicion_explain *out)
{
	if (fd == NULL || out == NULL)
		return -1;
	*out = (fd_suspicion_explain){0};
	out->player = who;
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL)
		return -1;
	fd_run(fd);

	Fact *s = fd_find_suspicion(fd, who);
	if (s == NULL)
		return 0;   /* self / dead / ejected / never sighted: no belief */

	/* Walk every (logodds-term) for this color -- the immutable evidence
	 * facts the rules asserted. Truncate the breakdown at the array
	 * bound; the totals below come from the suspicion fact and stay
	 * whole even then. */
	for (Fact *t = GetNextFactInTemplate(fd->logodds_term_tmpl, NULL);
	     t != NULL;
	     t = GetNextFactInTemplate(fd->logodds_term_tmpl, t)) {
		if (strcmp(fd_fact_symbol(t, "color"), cname) != 0)
			continue;
		if (out->n_terms >= FD_EXPLAIN_MAX_TERMS)
			continue;
		int i = out->n_terms++;
		snprintf(out->terms[i].source, sizeof out->terms[i].source,
		    "%s", fd_fact_symbol(t, "source"));
		snprintf(out->terms[i].key, sizeof out->terms[i].key,
		    "%s", fd_fact_symbol(t, "key"));
		out->terms[i].amount = fd_fact_int(t, "amount");
	}

	/* logodds_total / weight are authoritative from the suspicion fact
	 * (written by fd_normalize_suspicion), not re-summed here -- so a
	 * caller can cross-check the terms[] amounts against logodds_total. */
	out->logodds_total = fd_fact_int(s, "logodds");
	out->weight        = (int)fd_fact_int(s, "weight");
	out->likelihood    = fd_logistic_permille(out->logodds_total);
	return 1;
}

/* Telemetry: report the run-engine instrumentation fd_run accumulates.
 * Deliberately does NOT fd_run() first -- that would fire the drained
 * agenda and reset last_rules_fired to 0, hiding the very signal the
 * caller asked for. Only fact_count is read live. */
void
fd_run_stats_get(fd_detector *fd, fd_run_stats *out)
{
	if (out == NULL)
		return;
	*out = (fd_run_stats){0};
	if (fd == NULL || fd->env == NULL)
		return;
	out->last_rules_fired  = fd->last_rules_fired;
	out->total_rules_fired = fd->total_rules_fired;
	out->run_count         = fd->run_count;
	out->last_run_ms       = fd->last_run_ms;
	out->max_run_ms        = fd->max_run_ms;

	int n = 0;
	for (Fact *f = GetNextFact(fd->env, NULL); f != NULL;
	     f = GetNextFact(fd->env, f))
		n++;
	out->fact_count = n;
}

static fd_game_pressure_t
parse_pressure(const char *sym)
{
	if (strcmp(sym, "critical") == 0) return FD_PRESSURE_CRITICAL;
	if (strcmp(sym, "high") == 0)     return FD_PRESSURE_HIGH;
	if (strcmp(sym, "medium") == 0)   return FD_PRESSURE_MEDIUM;
	return FD_PRESSURE_LOW;
}

static const char *
pressure_name(fd_game_pressure_t p)
{
	switch (p) {
	case FD_PRESSURE_CRITICAL: return "critical";
	case FD_PRESSURE_HIGH:     return "high";
	case FD_PRESSURE_MEDIUM:   return "medium";
	case FD_PRESSURE_LOW:
	default:                   return "low";
	}
}

fd_game_pressure_t
fd_game_pressure_now(fd_detector *fd)
{
	if (fd == NULL)
		return FD_PRESSURE_LOW;
	Fact *gp = (fd->game_pressure_tmpl != NULL)
	    ? GetNextFactInTemplate(fd->game_pressure_tmpl, NULL)
	    : NULL;
	if (gp == NULL)
		return FD_PRESSURE_LOW;
	return parse_pressure(fd_fact_symbol(gp, "level"));
}

fd_game_pressure_t
fd_game_pressure(fd_detector *fd)
{
	if (fd == NULL)
		return FD_PRESSURE_LOW;
	fd_run(fd);
	return fd_game_pressure_now(fd);
}

/* Crew (town) win probability for a Mafia death process with `crew`
 * townsfolk and `imps` impostors, in per-mille. Each day the town
 * lynches a uniformly random live player (it has no information); each
 * night the impostors kill one crewmate. Town wins when impostors hit
 * 0, loses when crew <= impostors. Migdal 1009.1031: the parity-heavy
 * closed form -- here as a small recursive DP. State space is tiny
 * (<=16x16); recursion terminates because crew strictly decreases. */
static int
w_nm_permille(int crew, int imps)
{
	if (imps <= 0)
		return 1000;
	if (crew <= imps)
		return 0;
	int denom = crew + imps;
	/* Day lynch hits an impostor (prob imps/denom): if that was the
	 * last impostor the town has won; else the night kill follows. */
	int after_imp = (imps - 1 == 0)
	    ? 1000
	    : w_nm_permille(crew - 1, imps - 1);
	/* Day lynch hits a crewmate (prob crew/denom): the night kill then
	 * removes another crewmate. */
	int after_crew = ((crew - 1) <= imps)
	    ? 0
	    : w_nm_permille(crew - 2, imps);
	return (imps * after_imp + crew * after_crew) / denom;
}

int
fd_win_probability(fd_detector *fd)
{
	if (fd == NULL)
		return -1;
	fd_run(fd);
	Fact *r = fd_roster_fact(fd);
	if (r == NULL)
		return -1;
	int ac = (int)fd_fact_int(r, "alive-crew");
	int ai = (int)fd_fact_int(r, "alive-impostors");
	return w_nm_permille(ac, ai);
}

int
fd_vote_decision(fd_detector *fd, fd_vote_decision_t *out)
{
	if (fd == NULL || out == NULL)
		return -1;
	fd_run(fd);
	*out = (fd_vote_decision_t){0};

	/* Highest- and second-highest belief weight among alive non-self
	 * colors (those with a suspicion fact). */
	int      best = -1, second = -1;
	fd_player best_c = 0;
	int      alive_pool = 0;
	for (int c = 0; c < FD_MAX_PLAYERS; c++) {
		if (fd->self_color != SELF_UNSET && (fd_player)c == fd->self_color)
			continue;
		Fact *s = fd_find_suspicion(fd, (fd_player)c);
		if (s == NULL)
			continue;
		alive_pool++;
		int w = (int)fd_fact_int(s, "weight");
		if (w > best) {
			second = best;
			best   = w;
			best_c = (fd_player)c;
		} else if (w > second) {
			second = w;
		}
	}
	if (best < 0) {
		out->target         = best_c;
		out->recommendation = FD_VOTE_ABSTAIN;
		snprintf(out->rationale, sizeof out->rationale,
		    "no live suspects to vote on");
		return 1;
	}
	if (second < 0)
		second = 0;

	out->target     = best_c;
	out->suspicion  = best;
	out->confidence = best - second;

	/* The m/n prior: a uniform guess would land each alive color at
	 * alive-impostors / alive-pool. A best target at or below that is
	 * no better than chance. */
	Fact *r = fd_roster_fact(fd);
	int m = (r != NULL) ? (int)fd_fact_int(r, "alive-impostors") : 1;
	int prior = (alive_pool > 0) ? (m * 1000 / alive_pool) : 0;

	/* Expected value of casting: P(impostor)*gain - P(innocent)*loss,
	 * with the loss of a wrong ejection scaled DOWN as game pressure
	 * rises -- near parity a wrong skip loses the game outright, so a
	 * best-guess vote becomes correct. */
	fd_game_pressure_t gp = fd_game_pressure_now(fd);
	static const int LOSS_MULT[4] = { 4, 3, 2, 1 };  /* low..critical */
	int gp_idx = (int)gp;
	if (gp_idx < 0 || gp_idx > 3)
		gp_idx = 0;
	long long gain = 1000;
	long long loss = 1000LL * LOSS_MULT[gp_idx];
	long long ev   = (long long)best * gain
	    - (long long)(1000 - best) * loss;
	const int margin_bar = 80;

	if (best <= prior) {
		out->recommendation = FD_VOTE_SKIP;
		snprintf(out->rationale, sizeof out->rationale,
		    "no candidate exceeds the 1-in-%d prior (%d per mille)",
		    alive_pool, prior);
	} else if (ev > 0 && out->confidence >= margin_bar) {
		out->recommendation = FD_VOTE_CAST;
		snprintf(out->rationale, sizeof out->rationale,
		    "%s holds %d per-mille suspicion, %d clear of the field; "
		    "expected value positive at %s pressure",
		    fd_player_name(fd, best_c), best, out->confidence,
		    pressure_name(gp));
	} else {
		out->recommendation = FD_VOTE_ABSTAIN;
		snprintf(out->rationale, sizeof out->rationale,
		    "top two within the %d per-mille confidence margin; "
		    "distribution too flat to act on", margin_bar);
	}
	return 1;
}

/* ------------------------------------------------------------------
 * Positional queries
 * ------------------------------------------------------------------ */

int
fd_last_seen(fd_detector *fd, fd_player color,
    char *out_room, size_t bufsize, int *out_tick)
{
	if (fd == NULL || out_room == NULL || bufsize == 0 || out_tick == NULL)
		return -1;
	fd_run(fd);
	Fact *f = fd_find_crewmember(fd, color);
	if (f == NULL) {
		out_room[0] = '\0';
		*out_tick   = -1;
		return 0;
	}
	long long t = fd_fact_int(f, "last-seen-tick");
	if (t < 0) {
		out_room[0] = '\0';
		*out_tick   = -1;
		return 0;
	}
	snprintf(out_room, bufsize, "%s", fd_fact_symbol(f, "last-seen-room"));
	*out_tick = (int)t;
	return 1;
}

int
fd_room_occupants(fd_detector *fd, const char *room,
    fd_player *out_colors, int max)
{
	if (fd == NULL || out_colors == NULL || max <= 0)
		return -1;
	/* A NULL or empty room is not a usage error -- no room named means
	 * no occupants. Distinct from the -1 above (a caller mistake). */
	if (room == NULL || *room == '\0')
		return 0;
	fd_run(fd);

	/* Loop colors (not facts) so the result is in fd_player enum order
	 * regardless of game-time assertion order. */
	int written = 0;
	for (int c = 0; c < FD_MAX_PLAYERS && written < max; c++) {
		if (fd->self_color != SELF_UNSET &&
		    (fd_player)c == fd->self_color)
			continue;
		Fact *f = fd_find_crewmember(fd, (fd_player)c);
		if (f == NULL)
			continue;
		if (fd_get_status_now(fd, (fd_player)c) != FD_PLAYER_STATUS_ALIVE)
			continue;
		if (strcmp(fd_fact_symbol(f, "last-seen-room"), room) != 0)
			continue;
		out_colors[written++] = (fd_player)c;
	}
	return written;
}

void
fd_round_stats_get(fd_detector *fd, fd_round_stats *out)
{
	if (out == NULL)
		return;
	*out = (fd_round_stats){0};
	if (fd == NULL)
		return;
	fd_run(fd);

	int dossiered = 0;
	for (Fact *f = GetNextFactInTemplate(fd->crewmember_tmpl, NULL);
	     f != NULL;
	     f = GetNextFactInTemplate(fd->crewmember_tmpl, f)) {
		switch (fd_parse_status(fd_fact_symbol(f, "status"))) {
		case FD_PLAYER_STATUS_ALIVE:   out->alive++;   break;
		case FD_PLAYER_STATUS_DEAD:    out->dead++;    break;
		case FD_PLAYER_STATUS_EJECTED: out->ejected++; break;
		default:                                       break;
		}
		dossiered++;
	}
	/* never_sighted = players in the round with no dossier at all
	 * (other than self, which never gets a sighting-derived dossier).
	 * The universe is the configured roster (fd_observe_round_config),
	 * not the FD_MAX_PLAYERS id capacity — without a round config the
	 * library cannot know how many players exist, so it reports 0. */
	if (fd->cfg_n_players > 0) {
		int non_self = fd->cfg_n_players
		    - ((fd->self_color != SELF_UNSET) ? 1 : 0);
		out->never_sighted = non_self - dossiered;
		if (out->never_sighted < 0)
			out->never_sighted = 0;
	}

	out->open_cases      = fd_count_facts(fd->case_tmpl);
	out->vent_suspicions = fd_count_facts(fd->vent_suspected_tmpl);
	out->impostor_found  = fd_impostor_found(fd) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Safety projection: stance + status -> alone-with-X risk
 * ------------------------------------------------------------------ */

/* Stance-to-risk projection, indexed by [playstyle][stance]. Each row
 * is the risk for stance [UNKNOWN, ON_TASK, OFF_TASK, THREAT] (the
 * fd_stance enum order). The NEUTRAL row preserves the legacy mapping. */
static const fd_risk RISK_BY_STYLE[3][4] = {
	[FD_PLAYSTYLE_TRUSTING] =
	    { FD_RISK_LOW,    FD_RISK_LOW,    FD_RISK_LOW,    FD_RISK_HIGH },
	[FD_PLAYSTYLE_NEUTRAL] =
	    { FD_RISK_MEDIUM, FD_RISK_LOW,    FD_RISK_MEDIUM, FD_RISK_HIGH },
	[FD_PLAYSTYLE_PARANOID] =
	    { FD_RISK_MEDIUM, FD_RISK_MEDIUM, FD_RISK_HIGH,   FD_RISK_HIGH },
};

/* The table's shape is wired to the enum extents. If a playstyle or a
 * stance is ever added, these fail the build instead of letting
 * fd_alone_risk index past the end of a short row/column. */
_Static_assert(FD_PLAYSTYLE_PARANOID == 2,
    "RISK_BY_STYLE has 3 rows; fd_playstyle must span 0..2");
_Static_assert(FD_STANCE_THREAT == 3,
    "RISK_BY_STYLE rows have 4 columns; fd_stance must span 0..3");

fd_risk
fd_alone_risk(fd_detector *fd, fd_player who)
{
	if (fd == NULL)
		return FD_RISK_UNKNOWN;
	if (fd->self_color != SELF_UNSET && who == fd->self_color)
		return FD_RISK_NONE;

	fd_run(fd);

	fd_player_status status = fd_get_status_now(fd, who);
	if (status == FD_PLAYER_STATUS_DEAD ||
	    status == FD_PLAYER_STATUS_EJECTED)
		return FD_RISK_NONE;
	if (status == FD_PLAYER_STATUS_UNKNOWN) {
		/* Never sighted. A paranoid policy treats no-data as
		 * baseline suspicion; trusting/neutral honestly admit no
		 * read. */
		return (fd->style == FD_PLAYSTYLE_PARANOID)
		    ? FD_RISK_MEDIUM
		    : FD_RISK_UNKNOWN;
	}

	/* Status is alive. If EVERY impostor has already been ejected, no
	 * living crewmate is a killer (multi-impostor aware -- with the
	 * default single-impostor roster this matches fd_impostor_found). */
	if (fd_all_impostors_ejected(fd))
		return FD_RISK_NONE;

	fd_stance st = fd_get_stance_now(fd, who);
	if ((int)st < 0 || (int)st > FD_STANCE_THREAT)
		st = FD_STANCE_UNKNOWN;
	return RISK_BY_STYLE[fd->style][st];
}
