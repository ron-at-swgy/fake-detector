/*
 * fd_internal.h -- library-private declarations for libfakedetector.
 *
 * Shared between the library's translation units only. NOT installed
 * and NOT included by consumers (driver.c / perf.c use the public
 * fakedetector.h). It carries the detector struct and the handful of
 * helpers that cross module boundaries.
 */

#ifndef FD_INTERNAL_H
#define FD_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "fakedetector.h"

#include "clips.h"

#define SELF_UNSET   ((fd_player)-1)
#define FD_MAX_ROOMS 32

/* Two distinct "no path" sentinels, kept side by side so the contrast
 * is visible:
 *   FD_INF             -- the in-memory APSP-matrix sentinel. Used by
 *                         graph_recompute_apsp / fd_graph_path_cost.
 *   FD_UNREACHABLE_COST -- the value published into (room-distance)
 *                         facts for an unreachable pair. A round,
 *                         obviously-sentinel number, deliberately far
 *                         above any kill window (so the cast-iron-
 *                         alibi rule treats it as a perfect alibi)
 *                         yet well below FD_INF (so it never collides
 *                         with the matrix sentinel). */
#define FD_INF              INT32_MAX
#define FD_UNREACHABLE_COST 1000000000

/* Upper bound on a single room-link cost. A shortest path crosses at
 * most FD_MAX_ROOMS-1 links, so capping each link here keeps the
 * Floyd-Warshall path sums well inside int range -- no signed-overflow
 * UB in graph_recompute_apsp. One million ticks is ~11 hours of game
 * time; far beyond any real link. */
#define FD_MAX_LINK_COST 1000000

/* graph_recompute_apsp adds two shortest-path costs; each is at most
 * (FD_MAX_ROOMS-1) links of FD_MAX_LINK_COST. Enforce that the sum
 * cannot overflow a signed int, so the bound above is a guarantee and
 * not just a comment. */
_Static_assert((long long)FD_MAX_LINK_COST * FD_MAX_ROOMS * 2 <= INT32_MAX,
    "Floyd-Warshall path-sum addition must not overflow int");

/* Navigation graph for impossible-movement detection. Built up by
 * fd_add_room_link, modified at round granularity by fd_observe_route_*.
 * All-pairs shortest path is recomputed lazily on first vent check
 * after any mutation. */
struct room_graph {
	char *names[FD_MAX_ROOMS];   /* strdup'd; NULL = unused slot */
	int   n_rooms;
	int   direct[FD_MAX_ROOMS][FD_MAX_ROOMS];   /* base cost or FD_INF */
	bool  closed[FD_MAX_ROOMS][FD_MAX_ROOMS];   /* round-scoped closures */
	int   apsp[FD_MAX_ROOMS][FD_MAX_ROOMS];     /* shortest paths or FD_INF */
	bool  apsp_dirty;     /* apsp[][] needs recompute */
	bool  facts_dirty;    /* room-distance facts need republishing */
};

struct last_sighting {
	char *room;   /* strdup'd; NULL = no prior sighting this round */
	int   tick;
};

struct fd_detector {
	Environment         *env;
	Deftemplate         *stance_tmpl;
	Deftemplate         *self_state_tmpl;
	Deftemplate         *self_color_tmpl;
	Deftemplate         *crewmember_tmpl;
	Deftemplate         *impostor_found_tmpl;
	Deftemplate         *case_tmpl;
	Deftemplate         *kill_window_tmpl;
	Deftemplate         *alibi_tmpl;
	Deftemplate         *cast_iron_alibi_tmpl;
	Deftemplate         *suspect_tmpl;
	Deftemplate         *vent_suspected_tmpl;
	Deftemplate         *room_distance_tmpl;
	Deftemplate         *accusation_tmpl;
	Deftemplate         *suspicion_tmpl;
	Deftemplate         *logodds_term_tmpl;
	Deftemplate         *roster_tmpl;
	Deftemplate         *game_pressure_tmpl;
	fd_player            self_color;
	fd_playstyle         style;
	int                  cfg_n_players;    /* fd_observe_round_config; 0 = unset */
	int                  cfg_n_impostors;  /* fd_observe_round_config; default 1 */
	/* Display-name registry (fd_set_player_name). strdup'd; NULL slots
	 * fall back to the static "p<id>" defaults. Detector-scoped game
	 * config, like the room graph: persists across fd_reset. */
	char                *player_names[FD_MAX_PLAYERS];
	struct room_graph    graph;
	struct last_sighting last_sighting[FD_MAX_PLAYERS];
	/* Run-engine instrumentation (fd_run_stats_get). Counters are
	 * round-scoped: fd_run zeroes nothing, fd_reset clears them all. */
	long long            run_count;
	long long            last_rules_fired;
	long long            total_rules_fired;
	double               last_run_ms;
	double               max_run_ms;
	fd_ingest_stats      ingest_stats;
	/* Capturable trace (fd_trace_*). trace_stream NULL means inactive;
	 * the STDOUT-intercepting router is installed lazily and once.
	 * trace_stream_owned marks a stream the library fopen'd itself
	 * (fd_trace_begin_path) and must fclose at trace end / destroy. */
	FILE                *trace_stream;
	bool                 trace_stream_owned;
	bool                 trace_router_added;
};

enum fd_ingest_event {
	FD_INGEST_EVENT_ACCEPTED = 0,
	FD_INGEST_EVENT_VALIDATION_DROP,
	FD_INGEST_EVENT_SELF_FILTERED,
	FD_INGEST_EVENT_ASSERT_DROP
};

/* ------------------------------------------------------------------
 * fd_clips.c -- CLIPS fact-access glue
 * ------------------------------------------------------------------ */

bool             fd_assert_fact(fd_detector *fd, const char *fmt, ...);
bool             fd_symbol_ok(const char *s);
const char      *fd_fact_symbol(Fact *f, const char *slot);
long long        fd_fact_int(Fact *f, const char *slot);
Fact            *fd_find_crewmember(fd_detector *fd, fd_player who);
Fact            *fd_find_stance(fd_detector *fd, fd_player who);
Fact            *fd_find_suspicion(fd_detector *fd, fd_player who);
Fact            *fd_find_kill_window(fd_detector *fd, const char *victim);
Fact            *fd_roster_fact(fd_detector *fd);
Fact            *fd_latest_self_state(fd_detector *fd);
fd_player_status fd_parse_status(const char *sym);
fd_stance        fd_parse_category(const char *sym);
fd_evidence      fd_parse_evidence(const char *sym);
bool             fd_impostor_found(fd_detector *fd);
bool             fd_all_impostors_ejected(fd_detector *fd);
void             fd_normalize_suspicion(fd_detector *fd);
int              fd_count_facts(Deftemplate *tmpl);
int              fd_logistic_permille(long long milli_nats);
fd_stance        fd_get_stance_now(fd_detector *fd, fd_player who);
fd_evidence      fd_get_evidence_now(fd_detector *fd, fd_player who);
fd_player_status fd_get_status_now(fd_detector *fd, fd_player who);
int              fd_get_suspicion_now(fd_detector *fd, fd_player who);
fd_game_pressure_t fd_game_pressure_now(fd_detector *fd);

/* ------------------------------------------------------------------
 * fakedetector.c -- player-name helpers
 * ------------------------------------------------------------------ */

/* Reverse of fd_player_name: map a CLIPS player symbol back to its id.
 * -1 if unknown. Used when scanning facts whose player slot is a
 * symbol. */
int              fd_player_from_name(fd_detector *fd, const char *name);
void             fd_ingest_note_call(fd_detector *fd);
void             fd_ingest_note(fd_detector *fd, enum fd_ingest_event event);

/* ------------------------------------------------------------------
 * fd_graph.c -- navigation graph (cross-module entry points)
 * ------------------------------------------------------------------ */

int  fd_graph_path_cost(struct room_graph *g, const char *room_a,
         const char *room_b);
void fd_graph_reset_round_state(struct room_graph *g);
void fd_graph_publish_distances(fd_detector *fd);
void fd_last_sighting_clear(struct last_sighting cache[FD_MAX_PLAYERS]);
void fd_last_sighting_update(struct last_sighting *slot, const char *room,
         int tick);

/* ------------------------------------------------------------------
 * fd_telemetry.c -- output sink shared by the dump writers
 *
 * One writer, two destinations: a caller FILE * (fd_dump_state) or a
 * caller buffer with snprintf semantics (fd_dump_state_buf). `len`
 * accumulates the total bytes the output wants regardless of `cap`,
 * so a truncated buffer dump still reports its full size.
 * ------------------------------------------------------------------ */

struct fd_sink {
	FILE   *stream;   /* non-NULL: FILE-backed; buf/cap unused */
	char   *buf;      /* buffer-backed; may be NULL when cap == 0 */
	size_t  cap;
	size_t  len;
	bool    error;    /* a FILE-backed write failed */
};

void fd_sink_printf(struct fd_sink *s, const char *fmt, ...);

/* ------------------------------------------------------------------
 * fd_schema.c -- template validation + dump metadata
 * ------------------------------------------------------------------ */

bool fd_schema_cache(fd_detector *fd);
void fd_schema_dump_state(fd_detector *fd, struct fd_sink *sink);

/* ------------------------------------------------------------------
 * fd_rules_embedded.c (generated by mk/embed-clp.sh at build time)
 * -- the clp/ rule set baked into the library, in load order.
 * ------------------------------------------------------------------ */

struct fd_embedded_rule {
	const char          *name;   /* e.g. "templates.clp" */
	const unsigned char *data;
	size_t               len;
};

extern const struct fd_embedded_rule fd_embedded_rules[];
extern const size_t                  fd_embedded_rules_count;

#endif /* FD_INTERNAL_H */
