#include <stdio.h>

#include "fakedetector.h"

/* Test-local player ids (legacy roster order); default "p<id>" names. */
enum {
	P_RED = 0, P_YELLOW = 2, P_PINK = 4, P_BLUE = 6, P_GREEN = 13,
	P__COUNT = 16
};

static int
fail(const char *msg)
{
	fprintf(stderr, "test_query_run_contract: %s\n", msg);
	return 1;
}

static void
setup_graph(fd_detector *fd)
{
	fd_add_room_link(fd, "cafeteria", "admin", 50);
	fd_add_room_link(fd, "cafeteria", "medbay", 40);
	fd_add_room_link(fd, "medbay", "storage", 50);
	fd_add_room_link(fd, "medbay", "admin", 100);
}

static void
setup_round(fd_detector *fd)
{
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 5, 1);
	fd_observe_self(fd, 0, FD_PHASE_PREGAME, "cafeteria", 0, 0, 0);
	fd_observe_self(fd, 10, FD_PHASE_PLAYING, "cafeteria", 0, 0, 0);

	fd_observe_player(fd, 10, P_BLUE, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_PINK, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_YELLOW, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_GREEN, "cafeteria", 0, 0);

	fd_observe_player(fd, 40, P_BLUE, "cafeteria", 0, 0);
	fd_observe_task_completion(fd, 40, P_BLUE, "cafeteria");
	fd_observe_player(fd, 70, P_BLUE, "admin", 0, 0);
	fd_observe_task_completion(fd, 70, P_BLUE, "admin");

	fd_observe_player(fd, 80, P_YELLOW, "medbay", 0, 0);
	fd_observe_player(fd, 100, P_YELLOW, "admin", 0, 0);

	fd_observe_player(fd, 90, P_GREEN, "storage", 0, 0);
	fd_observe_player(fd, 110, P_PINK, "storage", 0, 0);
	fd_observe_body(fd, 120, P_GREEN, "storage", 0, 0);
}

static int
expect_one_run(fd_detector *fd, const char *label,
    void (*query_fn)(fd_detector *))
{
	fd_run_stats before, after;

	fd_run_stats_get(fd, &before);
	query_fn(fd);
	fd_run_stats_get(fd, &after);
	if (after.run_count == before.run_count + 1)
		return 0;

	fprintf(stderr, "%s: run_count delta = %lld, want 1\n",
	    label, after.run_count - before.run_count);
	return 1;
}

static void
run_pick_vote(fd_detector *fd)
{
	fd_player color = P_RED;
	fd_stance stance = FD_STANCE_UNKNOWN;

	(void)fd_pick_vote_target(fd, &color, &stance);
}

static void
run_rank_suspects(fd_detector *fd)
{
	fd_player colors[P__COUNT];
	fd_stance stances[P__COUNT];

	(void)fd_rank_suspects(fd, colors, stances, P__COUNT);
}

static void
run_room_occupants(fd_detector *fd)
{
	fd_player colors[P__COUNT];

	(void)fd_room_occupants(fd, "cafeteria", colors, P__COUNT);
}

static void
run_alone_risk(fd_detector *fd)
{
	(void)fd_alone_risk(fd, P_YELLOW);
}

static void
run_vote_summary(fd_detector *fd)
{
	char buf[2048];

	(void)fd_render_vote_summary(fd, buf, sizeof buf);
}

int
main(void)
{
	fd_detector *fd = fd_create("clp");
	if (fd == NULL)
		return fail("fd_create failed");

	setup_graph(fd);
	setup_round(fd);

	if (expect_one_run(fd, "fd_pick_vote_target", run_pick_vote) != 0 ||
	    expect_one_run(fd, "fd_rank_suspects", run_rank_suspects) != 0 ||
	    expect_one_run(fd, "fd_room_occupants", run_room_occupants) != 0 ||
	    expect_one_run(fd, "fd_alone_risk", run_alone_risk) != 0 ||
	    expect_one_run(fd, "fd_render_vote_summary", run_vote_summary) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_destroy(fd);
	puts("test_query_run_contract: OK");
	return 0;
}
