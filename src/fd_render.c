/*
 * fd_render.c -- player-facing prose builders.
 *
 * snprintf-style string output: the vote-time stance summary, the
 * Poirot-style case file, and the per-color rationale strings. These
 * turn the detector's facts into text suitable for chat or logging.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fd_internal.h"

/* snprintf-style append: writes into buf[offset..bufsize], returns the
 * number of bytes that *would* be written. Caller accumulates into total. */
static long
append(char *buf, size_t bufsize, long total, const char *fmt, ...)
{
	if (total < 0)
		return total;
	size_t off = (size_t)total;
	size_t room = (off < bufsize) ? (bufsize - off) : 0;
	char *dst = (buf != NULL && off < bufsize) ? (buf + off) : NULL;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, room, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	return total + n;
}

static const char *
stance_label(fd_stance s)
{
	switch (s) {
	case FD_STANCE_ON_TASK:  return "on-task";
	case FD_STANCE_OFF_TASK: return "off-task";
	case FD_STANCE_THREAT:   return "threat";
	default:                 return "unknown";
	}
}

static const char *
risk_label(fd_risk r)
{
	switch (r) {
	case FD_RISK_HIGH:    return "HIGH";
	case FD_RISK_MEDIUM:  return "MEDIUM";
	case FD_RISK_LOW:     return "LOW";
	case FD_RISK_NONE:    return "NONE";
	case FD_RISK_UNKNOWN:
	default:              return "UNKNOWN";
	}
}

static fd_risk
alone_risk_now(fd_detector *fd, fd_player who)
{
	static const fd_risk risk_by_style[3][4] = {
		[FD_PLAYSTYLE_TRUSTING] =
		    { FD_RISK_LOW, FD_RISK_LOW, FD_RISK_LOW, FD_RISK_HIGH },
		[FD_PLAYSTYLE_NEUTRAL] =
		    { FD_RISK_MEDIUM, FD_RISK_LOW, FD_RISK_MEDIUM,
		    FD_RISK_HIGH },
		[FD_PLAYSTYLE_PARANOID] =
		    { FD_RISK_MEDIUM, FD_RISK_MEDIUM, FD_RISK_HIGH,
		    FD_RISK_HIGH },
	};

	if (fd == NULL)
		return FD_RISK_UNKNOWN;
	if (fd->self_color != SELF_UNSET && who == fd->self_color)
		return FD_RISK_NONE;

	fd_player_status status = fd_get_status_now(fd, who);
	if (status == FD_PLAYER_STATUS_DEAD ||
	    status == FD_PLAYER_STATUS_EJECTED)
		return FD_RISK_NONE;
	if (status == FD_PLAYER_STATUS_UNKNOWN) {
		return (fd->style == FD_PLAYSTYLE_PARANOID)
		    ? FD_RISK_MEDIUM
		    : FD_RISK_UNKNOWN;
	}
	if (fd_all_impostors_ejected(fd))
		return FD_RISK_NONE;

	fd_stance st = fd_get_stance_now(fd, who);
	if ((int)st < 0 || (int)st > FD_STANCE_THREAT)
		st = FD_STANCE_UNKNOWN;
	return risk_by_style[fd->style][st];
}

static long
rationale_now(fd_detector *fd, fd_player who, char *buf, size_t bufsize)
{
	Fact *f = fd_find_stance(fd, who);
	const char *r = (f != NULL) ? fd_fact_symbol(f, "rationale") : "";
	int n = snprintf(buf, bufsize, "%s", r);
	return (n < 0) ? -1 : (long)n;
}

long
fd_get_rationale(fd_detector *fd, fd_player who, char *buf, size_t bufsize)
{
	if (fd == NULL || (buf == NULL && bufsize > 0))
		return -1;
	fd_run(fd);
	return rationale_now(fd, who, buf, bufsize);
}

/* Pick the explanation body for an alone-with-X rationale. Special
 * cases (self, deceased, impostor-found) take priority over the
 * stance rationale. Returns a literal, or fills stance_buf and
 * returns it. */
static const char *
alone_risk_body(fd_detector *fd, fd_player who, fd_risk r,
    char *stance_buf, size_t stance_bufsize)
{
	if (fd->self_color != SELF_UNSET && who == fd->self_color)
		return "that's you";
	if (r == FD_RISK_NONE && fd_all_impostors_ejected(fd))
		return "all impostors already ejected this round";

	fd_player_status status = fd_get_status_now(fd, who);
	if (status == FD_PLAYER_STATUS_DEAD)
		return "deceased";
	if (status == FD_PLAYER_STATUS_EJECTED)
		return "ejected";
	if (status == FD_PLAYER_STATUS_UNKNOWN)
		return "never sighted, no information";

	/* Alive crewmate with a stance -- reuse the stance rationale. */
	stance_buf[0] = '\0';
	rationale_now(fd, who, stance_buf, stance_bufsize);
	return (stance_buf[0] != '\0') ? stance_buf : "no specific evidence";
}

long
fd_alone_risk_rationale(fd_detector *fd, fd_player who, char *buf,
    size_t bufsize)
{
	if (fd == NULL || (buf == NULL && bufsize > 0))
		return -1;
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL)
		return -1;

	fd_run(fd);
	fd_risk r = alone_risk_now(fd, who);

	char stance_buf[256];
	const char *body = alone_risk_body(fd, who, r,
	    stance_buf, sizeof stance_buf);

	/* Non-neutral playstyles flag themselves so the caller knows the
	 * risk level was projected through a bias. Underlying evidence
	 * (`body`) stays objective. */
	const char *style_tag = "";
	if (fd->style == FD_PLAYSTYLE_TRUSTING)
		style_tag = " (trusting)";
	else if (fd->style == FD_PLAYSTYLE_PARANOID)
		style_tag = " (paranoid)";

	int n = snprintf(buf, bufsize, "alone with %s: %s risk%s - %s",
	    cname, risk_label(r), style_tag, body);
	return (n < 0) ? -1 : (long)n;
}

/* ------------------------------------------------------------------
 * fd_render_vote_summary -- one stance line per alive color.
 * ------------------------------------------------------------------ */

static const fd_stance CATEGORY_ORDER[] = {
	FD_STANCE_THREAT, FD_STANCE_OFF_TASK, FD_STANCE_ON_TASK, FD_STANCE_UNKNOWN
};

/* Append the summary header line; returns the running total. */
static long
render_summary_header(fd_detector *fd, char *buf, size_t bufsize, long total)
{
	const char *self_color_str = "?";
	const char *self_phase = "?";
	long long self_tick = -1;
	if (fd->self_color != SELF_UNSET)
		self_color_str = fd_player_name(fd, fd->self_color);
	Fact *ss = fd_latest_self_state(fd);
	if (ss != NULL) {
		self_phase = fd_fact_symbol(ss, "phase");
		self_tick  = fd_fact_int(ss, "tick");
	}
	return append(buf, bufsize, total,
	    "fake-detector verdicts (self=%s, tick=%lld, phase=%s):\n",
	    self_color_str, self_tick, self_phase);
}

/* Append the deceased footer; returns the running total. */
static long
render_deceased_footer(fd_detector *fd, char *buf, size_t bufsize, long total)
{
	bool any_deceased = false;
	for (int c = 0; c < FD_MAX_PLAYERS; c++) {
		fd_player_status st = fd_get_status_now(fd, (fd_player)c);
		if (st != FD_PLAYER_STATUS_DEAD &&
		    st != FD_PLAYER_STATUS_EJECTED)
			continue;
		if (!any_deceased)
			total = append(buf, bufsize, total, "  deceased:");
		total = append(buf, bufsize, total, " %s(%s)",
		    fd_player_name(fd, (fd_player)c),
		    st == FD_PLAYER_STATUS_DEAD ? "body" : "ejected");
		any_deceased = true;
	}
	if (any_deceased)
		total = append(buf, bufsize, total, "\n");
	return total;
}

long
fd_render_vote_summary(fd_detector *fd, char *buf, size_t bufsize)
{
	if (fd == NULL || (buf == NULL && bufsize > 0))
		return -1;
	fd_run(fd);

	long total = render_summary_header(fd, buf, bufsize, 0);

	bool any_stance = false;
	for (size_t i = 0;
	     i < sizeof CATEGORY_ORDER / sizeof CATEGORY_ORDER[0]; i++) {
		fd_stance cat = CATEGORY_ORDER[i];
		for (int c = 0; c < FD_MAX_PLAYERS; c++) {
			if (fd->self_color != SELF_UNSET &&
			    (fd_player)c == fd->self_color)
				continue;
			Fact *f = fd_find_stance(fd, (fd_player)c);
			if (f == NULL)
				continue;
			if (fd_parse_category(fd_fact_symbol(f, "category"))
			    != cat)
				continue;
			const char *rationale = fd_fact_symbol(f, "rationale");
			total = append(buf, bufsize, total,
			    "  %-11s %-8s %s\n",
			    fd_player_name(fd, (fd_player)c), stance_label(cat),
			    rationale[0] != '\0' ? rationale : "(no rationale)");
			any_stance = true;
		}
	}
	if (!any_stance)
		total = append(buf, bufsize, total,
		    "  (no stances available)\n");

	return render_deceased_footer(fd, buf, bufsize, total);
}

/* ------------------------------------------------------------------
 * fd_render_case -- Poirot-style case file per open case.
 * ------------------------------------------------------------------ */

/* Append the suspect roster for one case; returns the running total. */
static long
render_case_suspects(fd_detector *fd, char *buf, size_t bufsize, long total,
    const char *victim)
{
	bool any_suspect = false;
	for (Fact *s = GetNextFactInTemplate(fd->suspect_tmpl, NULL);
	     s != NULL;
	     s = GetNextFactInTemplate(fd->suspect_tmpl, s)) {
		if (strcmp(fd_fact_symbol(s, "victim"), victim) != 0)
			continue;
		const char *color = fd_fact_symbol(s, "color");
		const char *basis = fd_fact_symbol(s, "basis");
		if (!any_suspect)
			total = append(buf, bufsize, total, "  Suspects:\n");
		total = append(buf, bufsize, total,
		    "    %s - %s\n", color, basis);
		any_suspect = true;
	}
	if (!any_suspect)
		total = append(buf, bufsize, total,
		    "  Suspects: none (all alive crewmates alibied)\n");
	return total;
}

/* Find the alibi sighting (room + tick) backing a (color, victim)
 * pair, or NULL. Every cast-iron alibi has one -- derive-cast-iron-
 * alibi promotes an existing weak alibi. */
static Fact *
find_alibi(fd_detector *fd, const char *color, const char *victim)
{
	for (Fact *a = GetNextFactInTemplate(fd->alibi_tmpl, NULL);
	     a != NULL;
	     a = GetNextFactInTemplate(fd->alibi_tmpl, a)) {
		if (strcmp(fd_fact_symbol(a, "color"), color) == 0 &&
		    strcmp(fd_fact_symbol(a, "victim"), victim) == 0)
			return a;
	}
	return NULL;
}

/* Append the cleared roster for one case -- only CAST-IRON alibis
 * clear a suspect; a weak alibi merely tempers the basis (see
 * cases.clp). Returns the running total. */
static long
render_case_cleared(fd_detector *fd, char *buf, size_t bufsize, long total,
    const char *victim)
{
	bool any_clear = false;
	for (Fact *ci = GetNextFactInTemplate(fd->cast_iron_alibi_tmpl, NULL);
	     ci != NULL;
	     ci = GetNextFactInTemplate(fd->cast_iron_alibi_tmpl, ci)) {
		if (strcmp(fd_fact_symbol(ci, "victim"), victim) != 0)
			continue;
		const char *color = fd_fact_symbol(ci, "color");
		Fact *a = find_alibi(fd, color, victim);
		if (!any_clear)
			total = append(buf, bufsize, total, "  Cleared:\n");
		if (a != NULL)
			total = append(buf, bufsize, total,
			    "    %s - cast-iron alibi: sighted in %s at "
			    "tick %lld, too far from the scene\n",
			    color, fd_fact_symbol(a, "at-room"),
			    fd_fact_int(a, "at-tick"));
		else
			total = append(buf, bufsize, total,
			    "    %s - cast-iron alibi\n", color);
		any_clear = true;
	}
	return total;
}

/* Append the reveal line if the case has collapsed to one suspect.
 * Returns the running total. */
static long
render_case_reveal(fd_detector *fd, char *buf, size_t bufsize, long total,
    const char *victim)
{
	for (Fact *a = GetNextFactInTemplate(fd->accusation_tmpl, NULL);
	     a != NULL;
	     a = GetNextFactInTemplate(fd->accusation_tmpl, a)) {
		if (strcmp(fd_fact_symbol(a, "victim"), victim) != 0)
			continue;
		return append(buf, bufsize, total,
		    "  *** The field has narrowed to one. The murderer of "
		    "%s is %s. ***\n", victim, fd_fact_symbol(a, "color"));
	}
	return total;
}

/* Append one full case section (header, kill window, suspects,
 * cleared); returns the running total. */
static long
render_case_section(fd_detector *fd, char *buf, size_t bufsize, long total,
    Fact *c)
{
	const char *victim    = fd_fact_symbol(c, "victim");
	const char *room      = fd_fact_symbol(c, "room");
	long long   body_tick = fd_fact_int(c, "body-tick");

	total = append(buf, bufsize, total,
	    "Case file: the murder of %s\n", victim);
	total = append(buf, bufsize, total,
	    "  Body discovered in %s at tick %lld.\n", room, body_tick);

	Fact *kw = fd_find_kill_window(fd, victim);
	if (kw != NULL) {
		long long from_tick = fd_fact_int(kw, "from-tick");
		long long to_tick   = fd_fact_int(kw, "to-tick");
		total = append(buf, bufsize, total,
		    "  Kill window: ticks %lld..%lld.\n", from_tick, to_tick);
	}

	total = render_case_suspects(fd, buf, bufsize, total, victim);
	total = render_case_cleared(fd, buf, bufsize, total, victim);
	total = render_case_reveal(fd, buf, bufsize, total, victim);
	return total;
}

long
fd_render_case(fd_detector *fd, char *buf, size_t bufsize)
{
	if (fd == NULL || (buf == NULL && bufsize > 0))
		return -1;
	fd_run(fd);

	Fact *first_case = GetNextFactInTemplate(fd->case_tmpl, NULL);
	if (first_case == NULL)
		return append(buf, bufsize, 0,
		    "Case file: no bodies discovered yet.\n");

	long total = 0;
	for (Fact *c = first_case; c != NULL;
	     c = GetNextFactInTemplate(fd->case_tmpl, c)) {
		total = render_case_section(fd, buf, bufsize, total, c);
	}
	return total;
}
