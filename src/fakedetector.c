/*
 * fakedetector.c -- detector lifecycle, identity, and playstyle.
 *
 * The library's front door: create / destroy / reset, the self-color
 * identity, the playstyle setting, and the color-name accessor.
 * Feature behavior lives in the fd_* sibling modules; see fd_internal.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fd_internal.h"

const char *
fd_version(void)
{
	return FD_VERSION_STRING;
}

int
fd_abi_version(void)
{
	return FD_ABI_VERSION;
}

/* Default display names for ids never given one via fd_set_player_name.
 * Static so fd_player_name can hand them out with a stable lifetime. */
static const char *const DEFAULT_NAMES[FD_MAX_PLAYERS] = {
	"p0",  "p1",  "p2",  "p3",  "p4",  "p5",  "p6",  "p7",
	"p8",  "p9",  "p10", "p11", "p12", "p13", "p14", "p15",
	"p16", "p17", "p18", "p19", "p20", "p21", "p22", "p23",
	"p24", "p25", "p26", "p27", "p28", "p29", "p30", "p31"
};

const char *
fd_player_name(fd_detector *fd, fd_player id)
{
	if (fd == NULL || id < 0 || id >= FD_MAX_PLAYERS)
		return NULL;
	if (fd->player_names[id] != NULL)
		return fd->player_names[id];
	return DEFAULT_NAMES[id];
}

int
fd_set_player_name(fd_detector *fd, fd_player id, const char *name)
{
	if (fd == NULL)
		return -1;
	fd_ingest_note_call(fd);
	if (id < 0 || id >= FD_MAX_PLAYERS ||
	    name == NULL || !fd_symbol_ok(name)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return -1;
	}
	/* A duplicate symbol would silently alias two players in working
	 * memory; reject it. (Colliding with another id's "p<n>" default is
	 * fine — that id has no facts under the default name until it is
	 * observed, and naming players is an all-or-nothing convention.) */
	for (int i = 0; i < FD_MAX_PLAYERS; i++) {
		if (i != id && fd->player_names[i] != NULL &&
		    strcmp(fd->player_names[i], name) == 0) {
			fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
			return -1;
		}
	}
	char *copy = strdup(name);
	if (copy == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return -1;
	}
	free(fd->player_names[id]);
	fd->player_names[id] = copy;
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
	return 0;
}

int
fd_player_from_name(fd_detector *fd, const char *name)
{
	if (fd == NULL || name == NULL)
		return -1;
	/* Registered names first: if some id was registered as "p3", the
	 * registration owns that symbol — id 3's default must not shadow
	 * it. */
	for (int i = 0; i < FD_MAX_PLAYERS; i++) {
		if (fd->player_names[i] != NULL &&
		    strcmp(fd->player_names[i], name) == 0)
			return i;
	}
	for (int i = 0; i < FD_MAX_PLAYERS; i++) {
		if (fd->player_names[i] == NULL &&
		    strcmp(DEFAULT_NAMES[i], name) == 0)
			return i;
	}
	return -1;
}

void
fd_ingest_note_call(fd_detector *fd)
{
	if (fd == NULL)
		return;
	fd->ingest_stats.total_calls++;
}

void
fd_ingest_note(fd_detector *fd, enum fd_ingest_event event)
{
	if (fd == NULL)
		return;
	switch (event) {
	case FD_INGEST_EVENT_ACCEPTED:
		fd->ingest_stats.accepted_calls++;
		return;
	case FD_INGEST_EVENT_VALIDATION_DROP:
		fd->ingest_stats.validation_drops++;
		return;
	case FD_INGEST_EVENT_SELF_FILTERED:
		fd->ingest_stats.self_filtered_drops++;
		return;
	case FD_INGEST_EVENT_ASSERT_DROP:
		fd->ingest_stats.assert_drops++;
		return;
	}
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

static bool
load_one(Environment *env, const char *dir, const char *file)
{
	char path[512];
	int n = snprintf(path, sizeof path, "%s/%s", dir, file);
	if (n < 0 || (size_t)n >= sizeof path)
		return false;
	return Load(env, path) == LE_NO_ERROR;
}

/* Load the rule set baked in at build time (mk/embed-clp.sh), in the
 * same canonical order as the disk path below. */
static bool
load_embedded(Environment *env)
{
	for (size_t i = 0; i < fd_embedded_rules_count; i++) {
		const struct fd_embedded_rule *r = &fd_embedded_rules[i];
		if (!LoadFromString(env, (const char *)r->data, r->len))
			return false;
	}
	return true;
}

/* Fill a room-cost matrix: `diag` on the diagonal, `offdiag` elsewhere. */
static void
fill_cost_matrix(int m[FD_MAX_ROOMS][FD_MAX_ROOMS], int diag, int offdiag)
{
	for (int i = 0; i < FD_MAX_ROOMS; i++)
		for (int j = 0; j < FD_MAX_ROOMS; j++)
			m[i][j] = (i == j) ? diag : offdiag;
}

/* Both cost matrices start at FD_INF off-diagonal, 0 on the diagonal
 * (0 off-diagonal would mean "instant teleport"). closed[][] and the
 * dirty flags stay zeroed by the caller's calloc. */
static void
init_graph(struct room_graph *g)
{
	fill_cost_matrix(g->direct, 0, FD_INF);
	fill_cost_matrix(g->apsp,   0, FD_INF);
}

fd_detector *
fd_create(const char *clp_dir)
{
	fd_detector *fd = calloc(1, sizeof *fd);
	if (fd == NULL)
		return NULL;
	fd->self_color      = SELF_UNSET;
	fd->style           = FD_PLAYSTYLE_NEUTRAL;
	fd->cfg_n_players   = 0;
	fd->cfg_n_impostors = 1;
	init_graph(&fd->graph);

	fd->env = CreateEnvironment();
	if (fd->env == NULL) {
		free(fd);
		return NULL;
	}

	/* NULL: the embedded rule set (no disk access). A directory: load
	 * from disk, for rule iteration without rebuilding the library. */
	bool loaded = (clp_dir == NULL)
	    ? load_embedded(fd->env)
	    : (load_one(fd->env, clp_dir, "templates.clp") &&
	       load_one(fd->env, clp_dir, "dossier.clp") &&
	       load_one(fd->env, clp_dir, "cases.clp") &&
	       load_one(fd->env, clp_dir, "claims.clp") &&
	       load_one(fd->env, clp_dir, "social.clp") &&
	       load_one(fd->env, clp_dir, "suspicion.clp") &&
	       load_one(fd->env, clp_dir, "stance.clp"));

	if (!loaded || !fd_schema_cache(fd)) {
		DestroyEnvironment(fd->env);
		free(fd);
		return NULL;
	}

	Reset(fd->env);
	return fd;
}

void
fd_destroy(fd_detector *fd)
{
	if (fd == NULL)
		return;
	if (fd->env != NULL)
		DestroyEnvironment(fd->env);
	if (fd->trace_stream != NULL && fd->trace_stream_owned)
		fclose(fd->trace_stream);
	for (int i = 0; i < FD_MAX_PLAYERS; i++)
		free(fd->player_names[i]);
	for (int i = 0; i < FD_MAX_ROOMS; i++)
		free(fd->graph.names[i]);
	fd_last_sighting_clear(fd->last_sighting);
	free(fd);
}

void
fd_reset(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return;
	Reset(fd->env);
	fd->self_color = SELF_UNSET;
	/* Round config is re-asserted by the caller after fd_reset (same
	 * contract as fd_set_self); fall back to a single impostor until
	 * then. */
	fd->cfg_n_players   = 0;
	fd->cfg_n_impostors = 1;
	/* Round-scoped state: closures reopen, prior sightings clear. The
	 * base graph (names + direct[]) persists across rounds. */
	fd_graph_reset_round_state(&fd->graph);
	fd_last_sighting_clear(fd->last_sighting);
	/* Run-engine instrumentation is round-scoped. An active trace is
	 * NOT — fd_trace_* lifetime is the caller's, and a dev trace may
	 * deliberately span rounds. */
	fd->run_count         = 0;
	fd->last_rules_fired  = 0;
	fd->total_rules_fired = 0;
	fd->last_run_ms       = 0.0;
	fd->max_run_ms        = 0.0;
}

/* ------------------------------------------------------------------
 * Identity and playstyle
 * ------------------------------------------------------------------ */

void
fd_set_self(fd_detector *fd, fd_player self)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	const char *cname = fd_player_name(fd, self);
	if (cname == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}

	/* Replace any existing (self-color) fact. */
	Fact *existing = GetNextFactInTemplate(fd->self_color_tmpl, NULL);
	if (existing != NULL)
		Retract(existing);

	fd->self_color = self;
	if (!fd_assert_fact(fd, "(self-color (color %s))", cname)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_ASSERT_DROP);
		return;
	}
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_set_playstyle(fd_detector *fd, fd_playstyle style)
{
	if (fd == NULL)
		return;
	if ((int)style < (int)FD_PLAYSTYLE_TRUSTING)
		style = FD_PLAYSTYLE_TRUSTING;
	else if ((int)style > (int)FD_PLAYSTYLE_PARANOID)
		style = FD_PLAYSTYLE_PARANOID;
	fd->style = style;
}

fd_playstyle
fd_get_playstyle(fd_detector *fd)
{
	if (fd == NULL)
		return FD_PLAYSTYLE_NEUTRAL;
	return fd->style;
}
