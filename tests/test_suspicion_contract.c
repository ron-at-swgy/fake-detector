#include <stdio.h>

#include "fd_internal.h"

/* Test-local player ids in the legacy roster order (the tie-break
 * expectations below depend on these numeric values), with the color
 * names registered so the white-box (suspicion (color blue)) asserts
 * match the symbols the library mints. */
enum {
	P_RED = 0, P_YELLOW = 2, P_PINK = 4, P_BLUE = 6, P_GREEN = 13,
	P__COUNT = 16
};

static void
register_players(fd_detector *fd)
{
	fd_set_player_name(fd, P_RED, "red");
	fd_set_player_name(fd, P_YELLOW, "yellow");
	fd_set_player_name(fd, P_PINK, "pink");
	fd_set_player_name(fd, P_BLUE, "blue");
	fd_set_player_name(fd, P_GREEN, "green");
}

static int
fail(const char *msg)
{
	fprintf(stderr, "test_suspicion_contract: %s\n", msg);
	return 1;
}

static int
sum_public_suspicion(fd_detector *fd)
{
	int sum = 0;

	for (int c = 0; c < P__COUNT; c++) {
		int w = fd_get_suspicion(fd, (fd_player)c);
		if (w >= 0)
			sum += w;
	}
	return sum;
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
setup_weighted_round(fd_detector *fd)
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

static void
setup_flat_round(fd_detector *fd)
{
	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 4, 1);
	fd_observe_self(fd, 0, FD_PHASE_PLAYING, "cafeteria", 0, 0, 0);
	fd_observe_player(fd, 10, P_BLUE, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_PINK, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_YELLOW, "cafeteria", 0, 0);
}

static int
expect_public_sum(fd_detector *fd, const char *label)
{
	int sum = sum_public_suspicion(fd);
	if (sum == 1000)
		return 0;
	fprintf(stderr, "%s: suspicion sum = %d, want 1000\n", label, sum);
	return 1;
}

int
main(void)
{
	fd_detector *fd = fd_create("clp");
	if (fd == NULL)
		return fail("fd_create failed");

	register_players(fd);
	setup_graph(fd);
	setup_weighted_round(fd);
	if (expect_public_sum(fd, "weighted round") != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_ejection(fd, 130, P_YELLOW, 0);
	if (expect_public_sum(fd, "post-ejection round") != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_reset(fd);
	setup_flat_round(fd);
	if (expect_public_sum(fd, "flat round") != 0) {
		fd_destroy(fd);
		return 1;
	}

	int blue = fd_get_suspicion(fd, P_BLUE);
	int pink = fd_get_suspicion(fd, P_PINK);
	int yellow = fd_get_suspicion(fd, P_YELLOW);
	if (blue < 0 || pink < 0 || yellow < 0) {
		fd_destroy(fd);
		return fail("flat round did not publish suspicion facts");
	}
	if ((blue > pink ? blue - pink : pink - blue) > 1 ||
	    (blue > yellow ? blue - yellow : yellow - blue) > 1 ||
	    (pink > yellow ? pink - yellow : yellow - pink) > 1) {
		fd_destroy(fd);
		return fail("flat round weights diverged by more than one");
	}

	fd_reset(fd);
	if (!fd_assert_fact(fd, "(suspicion (color blue))") ||
	    !fd_assert_fact(fd, "(suspicion (color pink))") ||
	    !fd_assert_fact(fd, "(suspicion (color yellow))")) {
		fd_destroy(fd);
		return fail("white-box suspicion setup failed");
	}
	fd_normalize_suspicion(fd);

	int sum = 0;
	for (Fact *s = GetNextFactInTemplate(fd->suspicion_tmpl, NULL);
	     s != NULL;
	     s = GetNextFactInTemplate(fd->suspicion_tmpl, s)) {
		sum += (int)fd_fact_int(s, "weight");
	}
	if (sum != 1000) {
		fd_destroy(fd);
		return fail("all-zero fallback did not sum to 1000");
	}
	if (fd_get_suspicion_now(fd, P_YELLOW) != 334 ||
	    fd_get_suspicion_now(fd, P_PINK) != 333 ||
	    fd_get_suspicion_now(fd, P_BLUE) != 333) {
		fd_destroy(fd);
		return fail("all-zero fallback did not break ties by color order");
	}

	fd_destroy(fd);
	puts("test_suspicion_contract: OK");
	return 0;
}
