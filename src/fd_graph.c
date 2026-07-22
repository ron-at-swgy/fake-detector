/*
 * fd_graph.c -- room navigation graph for impossible-movement detection.
 *
 * Owns the caller-asserted room graph, its all-pairs shortest-path
 * matrix, the round-scoped closure overlay, and the per-color
 * last-sighting cache that the vent check compares against.
 */

#include <stdlib.h>
#include <string.h>

#include "fd_internal.h"

/* Look up a room name in the graph. Returns the index or -1 if not
 * registered. Linear scan; n_rooms <= FD_MAX_ROOMS = 32. */
static int
graph_find_room(const struct room_graph *g, const char *name)
{
	if (name == NULL)
		return -1;
	for (int i = 0; i < g->n_rooms; i++) {
		if (g->names[i] != NULL && strcmp(g->names[i], name) == 0)
			return i;
	}
	return -1;
}

/* Look up or intern a room name. Returns the index, or -1 on
 * allocation failure or FD_MAX_ROOMS exhaustion. */
static int
graph_intern_room(struct room_graph *g, const char *name)
{
	int idx = graph_find_room(g, name);
	if (idx >= 0)
		return idx;
	if (g->n_rooms >= FD_MAX_ROOMS)
		return -1;
	char *copy = strdup(name);
	if (copy == NULL)
		return -1;
	idx = g->n_rooms++;
	g->names[idx] = copy;
	/* New row/column: no direct edges yet, no closures. */
	for (int j = 0; j < FD_MAX_ROOMS; j++) {
		g->direct[idx][j] = (j == idx) ? 0 : FD_INF;
		g->direct[j][idx] = (j == idx) ? 0 : FD_INF;
		g->closed[idx][j] = false;
		g->closed[j][idx] = false;
	}
	g->apsp_dirty = true;
	return idx;
}

/* Floyd-Warshall against the effective edge matrix (closure overlay
 * applied). n_rooms <= 32 keeps this trivially cheap. */
static void
graph_recompute_apsp(struct room_graph *g)
{
	int n = g->n_rooms;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			g->apsp[i][j] = g->closed[i][j]
			    ? FD_INF
			    : g->direct[i][j];
		}
	}
	/* Floyd-Warshall relaxation: k ranges over candidate intermediate
	 * vertices; for every (i,j), a path detouring through k may beat
	 * the best cost known so far. */
	for (int k = 0; k < n; k++) {
		for (int i = 0; i < n; i++) {
			if (g->apsp[i][k] == FD_INF)
				continue;
			for (int j = 0; j < n; j++) {
				if (g->apsp[k][j] == FD_INF)
					continue;
				int via = g->apsp[i][k] + g->apsp[k][j];
				if (via < g->apsp[i][j])
					g->apsp[i][j] = via;
			}
		}
	}
	g->apsp_dirty = false;
}

/* Return the shortest-path cost from room_a to room_b, or FD_INF if
 * either room is unregistered or unreachable. Recomputes APSP lazily
 * if the graph has been mutated. */
int
fd_graph_path_cost(struct room_graph *g, const char *room_a,
    const char *room_b)
{
	int a = graph_find_room(g, room_a);
	int b = graph_find_room(g, room_b);
	if (a < 0 || b < 0)
		return FD_INF;
	if (g->apsp_dirty)
		graph_recompute_apsp(g);
	return g->apsp[a][b];
}

/* Public read-only query: shortest-path tick cost between two rooms,
 * closures applied. 0 for the same known room, -1 for unknown /
 * unreachable / bad args. Thin wrapper over fd_graph_path_cost (which
 * yields 0 for a room against itself and FD_INF otherwise). */
int
fd_room_distance(fd_detector *fd, const char *room_a, const char *room_b)
{
	if (fd == NULL || room_a == NULL || room_b == NULL)
		return -1;
	int cost = fd_graph_path_cost(&fd->graph, room_a, room_b);
	return (cost == FD_INF) ? -1 : cost;
}

/* Clear the closure overlay. Called by fd_reset. Preserves names[],
 * n_rooms, direct[][]. */
void
fd_graph_reset_round_state(struct room_graph *g)
{
	for (int i = 0; i < FD_MAX_ROOMS; i++) {
		for (int j = 0; j < FD_MAX_ROOMS; j++) {
			g->closed[i][j] = false;
		}
	}
	g->apsp_dirty  = true;
	g->facts_dirty = true;
}

/* Republish the all-pairs shortest-path costs as (room-distance ...)
 * facts. Retracts the stale set first. Called from fd_run when the
 * graph has changed (facts_dirty). */
void
fd_graph_publish_distances(fd_detector *fd)
{
	struct room_graph *g = &fd->graph;
	if (g->apsp_dirty)
		graph_recompute_apsp(g);

	Fact *f;
	while ((f = GetNextFactInTemplate(fd->room_distance_tmpl, NULL)) != NULL)
		Retract(f);

	for (int i = 0; i < g->n_rooms; i++) {
		for (int j = 0; j < g->n_rooms; j++) {
			if (i == j)
				continue;
			int c = g->apsp[i][j];
			fd_assert_fact(fd,
			    "(room-distance (from %s) (to %s) (cost %d))",
			    g->names[i], g->names[j],
			    (c == FD_INF) ? FD_UNREACHABLE_COST : c);
		}
	}
	g->facts_dirty = false;
}

void
fd_last_sighting_clear(struct last_sighting cache[FD_MAX_PLAYERS])
{
	for (int i = 0; i < FD_MAX_PLAYERS; i++) {
		free(cache[i].room);
		cache[i].room = NULL;
		cache[i].tick = 0;
	}
}

void
fd_last_sighting_update(struct last_sighting *slot, const char *room,
    int tick)
{
	free(slot->room);
	slot->room = strdup(room);  /* NULL on failure is OK -- next check skips */
	slot->tick = tick;
}

void
fd_add_room_link(fd_detector *fd, const char *room_a, const char *room_b,
    int cost_ticks)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (cost_ticks <= 0 || cost_ticks > FD_MAX_LINK_COST) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	/* Room names must be CLIPS-symbol-safe -- a sighting names its
	 * room the same way, and only safe names can ever match. */
	if (!fd_symbol_ok(room_a) || !fd_symbol_ok(room_b)) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	int a = graph_intern_room(&fd->graph, room_a);
	int b = graph_intern_room(&fd->graph, room_b);
	if (a < 0 || b < 0 || a == b) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	fd->graph.direct[a][b] = cost_ticks;
	fd->graph.direct[b][a] = cost_ticks;
	fd->graph.apsp_dirty  = true;
	fd->graph.facts_dirty = true;
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_route_closed(fd_detector *fd, const char *room_a,
    const char *room_b)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (room_a == NULL || room_b == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	int a = graph_find_room(&fd->graph, room_a);
	int b = graph_find_room(&fd->graph, room_b);
	if (a < 0 || b < 0) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (fd->graph.direct[a][b] == FD_INF)
		return;  /* no link to close */
	if (fd->graph.closed[a][b])
		return;  /* already closed -- idempotent */
	fd->graph.closed[a][b] = true;
	fd->graph.closed[b][a] = true;
	fd->graph.apsp_dirty  = true;
	fd->graph.facts_dirty = true;
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}

void
fd_observe_route_opened(fd_detector *fd, const char *room_a,
    const char *room_b)
{
	if (fd == NULL)
		return;
	fd_ingest_note_call(fd);
	if (room_a == NULL || room_b == NULL) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	int a = graph_find_room(&fd->graph, room_a);
	int b = graph_find_room(&fd->graph, room_b);
	if (a < 0 || b < 0) {
		fd_ingest_note(fd, FD_INGEST_EVENT_VALIDATION_DROP);
		return;
	}
	if (!fd->graph.closed[a][b])
		return;  /* already open -- idempotent */
	fd->graph.closed[a][b] = false;
	fd->graph.closed[b][a] = false;
	fd->graph.apsp_dirty  = true;
	fd->graph.facts_dirty = true;
	fd_ingest_note(fd, FD_INGEST_EVENT_ACCEPTED);
}
