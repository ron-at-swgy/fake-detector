/*
 * fd_observe.c -- observation ingestion.
 *
 * The fd_observe_* family. Each call translates a game event into a
 * CLIPS fact assertion. fd_observe_player additionally runs the
 * routing-distance vent check before recording the sighting.
 */

#include <string.h>

#include "fd_internal.h"

static const char *const PHASE_NAMES[] = {
	"pregame", "playing", "voting", "results", "gameover"
};

static const char *
phase_name(fd_phase p)
{
	if ((int)p < 0 ||
	    (int)p >= (int)(sizeof PHASE_NAMES / sizeof PHASE_NAMES[0]))
		return "unknown";
	return PHASE_NAMES[p];
}

static const char *
safe_room(const char *room)
{
	return (room != NULL && *room != '\0') ? room : "nowhere";
}

/* A room argument is usable iff it is absent (NULL/empty -- safe_room
 * maps it to the literal "nowhere") or a CLIPS-symbol-safe name. A
 * non-empty room with stray characters is rejected: interpolating it
 * raw into a fact string would corrupt the s-expression. Observe
 * calls with an unacceptable room are a documented no-op. */
static bool
room_acceptable(const char *room)
{
	return room == NULL || room[0] == '\0' || fd_symbol_ok(room);
}

void
fd_observe_self(fd_detector *fd, int tick, fd_phase phase, const char *room,
    int x, int y, int kill_ready)
{
	(void)x; (void)y; /* coords not part of self-state slot set */
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(self-state (tick %d) (phase %s) (room %s) (kill-ready %s))",
	    tick, phase_name(phase), safe_room(room),
	    kill_ready ? "t" : "nil")) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

/* Impossible-movement check: compare elapsed ticks against the
 * shortest-path walking cost from the player's prior room to the new
 * room. No graph or no prior sighting -> no check. */
static bool
vent_check(fd_detector *fd, fd_player who, const char *cname,
    const char *new_room, int tick)
{
	bool assert_drop = false;
	struct last_sighting *prev = &fd->last_sighting[(int)who];
	if (prev->room != NULL && tick >= prev->tick &&
	    strcmp(prev->room, new_room) != 0) {
		int cost = fd_graph_path_cost(&fd->graph, prev->room, new_room);
		int delta = tick - prev->tick;
		if (cost != FD_INF && cost > delta) {
			if (!fd_assert_fact(fd,
			    "(vent-suspected (color %s) (from-room %s) "
			    "(to-room %s) (delta-tick %d) (tick %d) "
			    "(basis inferred))",
			    cname, prev->room, new_room, delta, tick))
				assert_drop = true;
		}
	}
	fd_last_sighting_update(prev, new_room, tick);
	return assert_drop;
}

void
fd_observe_player(fd_detector *fd, int tick, fd_player who, const char *room,
    int x, int y)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	/* Defensive: silently drop sightings of self. The dossier rules
	 * key on sighting facts, so leaking self in would create a
	 * crewmember dossier for self. */
	if (fd->self_color != SELF_UNSET && who == fd->self_color) {
		fd_ingest_note(fd, FD_INGEST_EVENT_SELF_FILTERED);
		return;
	}
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}

	const char *new_room = safe_room(room);
	if (vent_check(fd, who, cname, new_room, tick))
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);

	if (!fd_assert_fact(fd,
	    "(sighting (tick %d) (color %s) (room %s) (x %d) (y %d))",
	    tick, cname, new_room, x, y)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

/* Directly observed vent use -- the confirmed counterpart to the
 * inferred routing-distance check in vent_check. Asserts a
 * (vent-suspected ... (basis observed)) fact; from-room and to-room
 * are both the vent's room. Self is dropped, as in fd_observe_player. */
void
fd_observe_vent(fd_detector *fd, int tick, fd_player who, const char *room)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (fd->self_color != SELF_UNSET && who == fd->self_color) {
		fd_ingest_note(fd, FD_INGEST_EVENT_SELF_FILTERED);
		return;
	}
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	const char *vroom = safe_room(room);
	if (!fd_assert_fact(fd,
	    "(vent-suspected (color %s) (from-room %s) (to-room %s) "
	    "(delta-tick 0) (tick %d) (basis observed))",
	    cname, vroom, vroom, tick)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_body(fd_detector *fd, int tick, fd_player color, const char *room,
    int x, int y)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	const char *cname = fd_player_name(fd, color);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(body-seen (tick %d) (color %s) (room %s) (x %d) (y %d))",
	    tick, cname, safe_room(room), x, y)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_task_completion(fd_detector *fd, int tick, fd_player who,
    const char *room)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(task-tick (tick %d) (color %s) (room %s))",
	    tick, cname, safe_room(room))) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_death(fd_detector *fd, int tick, fd_player who)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd, "(player-dead (color %s) (tick %d))",
	    cname, tick)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_ejection(fd_detector *fd, int tick, fd_player who, int was_impostor)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	const char *cname = fd_player_name(fd, who);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(ejection (color %s) (tick %d) (was-impostor %s))",
	    cname, tick, was_impostor ? "t" : "nil")) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

/* Round configuration: total player count and impostor count, both
 * common knowledge from round start. Replaces any (roster) fact already
 * in working memory -- so a re-call within a round, or a call after
 * fd_reset, cleanly re-establishes the count model. Counts are clamped
 * to sane floors. When this is never called the detector keeps the
 * single-impostor default established at fd_create / fd_reset. */
void
fd_observe_round_config(fd_detector *fd, int n_players, int n_impostors)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (n_impostors < 1)
		n_impostors = 1;
	if (n_players < 0)
		n_players = 0;
	fd->cfg_n_players   = n_players;
	fd->cfg_n_impostors = n_impostors;
	/* Retract any existing roster, then assert the configured one;
	 * roster-recount keeps maintaining the derived slots from here. */
	Fact *r = fd_roster_fact(fd);
	if (r != NULL)
		Retract(r);
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
	if (!fd_assert_fact(fd,
	    "(roster (n-players %d) (n-impostors %d))",
	    n_players, n_impostors))
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
}

/* Claim-kind enum -> CLIPS symbol. Order matches fd_claim_kind. */
static const char *const CLAIM_KIND_NAMES[] = {
	"location", "task", "self-report"
};

static const char *
claim_kind_name(fd_claim_kind k)
{
	if ((int)k < 0 ||
	    (int)k >= (int)(sizeof CLAIM_KIND_NAMES / sizeof CLAIM_KIND_NAMES[0]))
		return NULL;
	return CLAIM_KIND_NAMES[k];
}

/* A vote-chat claim the policy already parsed. Asserts a (claim ...)
 * fact; claims.clp checks it against the sighting record. Self is
 * dropped, as in fd_observe_player. */
void
fd_observe_claim(fd_detector *fd, int tick, fd_player who, fd_claim_kind kind,
    const char *room)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (!room_acceptable(room)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (fd->self_color != SELF_UNSET && who == fd->self_color) {
		fd_ingest_note(fd, FD_INGEST_EVENT_SELF_FILTERED);
		return;
	}
	const char *cname = fd_player_name(fd, who);
	const char *kname = claim_kind_name(kind);
	if (cname == NULL || kname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(claim (color %s) (kind %s) (room %s) (tick %d))",
	    cname, kname, safe_room(room), tick)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

/* A vote / accusation edge for the social graph. Asserts (accuses ...).
 * A vote by self is dropped -- self's own votes are not evidence to
 * mine about self. */
void
fd_observe_vote(fd_detector *fd, int tick, fd_player voter, fd_player target)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (fd->self_color != SELF_UNSET && voter == fd->self_color) {
		fd_ingest_note(fd, FD_INGEST_EVENT_SELF_FILTERED);
		return;
	}
	const char *vname = fd_player_name(fd, voter);
	const char *tname = fd_player_name(fd, target);
	if (vname == NULL || tname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(accuses (a %s) (b %s) (tick %d))", vname, tname, tick)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

/* A defense edge for the social graph. Asserts (defends ...). A defense
 * by self is dropped. */
void
fd_observe_defense(fd_detector *fd, int tick, fd_player defender,
    fd_player defended)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (fd->self_color != SELF_UNSET && defender == fd->self_color) {
		fd_ingest_note(fd, FD_INGEST_EVENT_SELF_FILTERED);
		return;
	}
	const char *dname = fd_player_name(fd, defender);
	const char *bname = fd_player_name(fd, defended);
	if (dname == NULL || bname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd_assert_fact(fd,
	    "(defends (a %s) (b %s) (tick %d))", dname, bname, tick)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}
