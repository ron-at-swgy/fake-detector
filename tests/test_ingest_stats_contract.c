#include <stdio.h>
#include <string.h>

#include "fakedetector.h"

/* Test-local player ids (legacy roster order); default "p<id>" names. */
enum { P_RED = 0, P_BLUE = 6 };

static int
failf(const char *msg, long long got, long long want)
{
	fprintf(stderr,
	    "test_ingest_stats_contract: %s (got %lld, want %lld)\n",
	    msg, got, want);
	return 1;
}

static int
expect_stats(fd_detector *fd, long long total, long long accepted,
    long long validation, long long self_filtered, long long assert_drops)
{
	fd_ingest_stats stats;

	fd_ingest_stats_get(fd, &stats);
	if (stats.total_calls != total)
		return failf("total_calls mismatch", stats.total_calls, total);
	if (stats.accepted_calls != accepted)
		return failf("accepted_calls mismatch", stats.accepted_calls, accepted);
	if (stats.validation_drops != validation)
		return failf("validation_drops mismatch", stats.validation_drops,
		    validation);
	if (stats.self_filtered_drops != self_filtered)
		return failf("self_filtered_drops mismatch",
		    stats.self_filtered_drops, self_filtered);
	if (stats.assert_drops != assert_drops)
		return failf("assert_drops mismatch", stats.assert_drops,
		    assert_drops);
	return 0;
}

int
main(void)
{
	fd_detector *fd = fd_create("clp");
	char long_room[700];

	if (fd == NULL) {
		fprintf(stderr, "test_ingest_stats_contract: fd_create failed\n");
		return 1;
	}

	memset(long_room, 'a', sizeof long_room - 1);
	long_room[sizeof long_room - 1] = '\0';

	if (expect_stats(fd, 0, 0, 0, 0, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_set_self(fd, (fd_player)-1);
	if (expect_stats(fd, 1, 0, 1, 0, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_set_self(fd, P_RED);
	if (expect_stats(fd, 2, 1, 1, 0, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_player(fd, 10, P_RED, "cafeteria", 0, 0);
	if (expect_stats(fd, 3, 1, 1, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_body(fd, 10, P_BLUE, "bad room!", 0, 0);
	if (expect_stats(fd, 4, 1, 2, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_add_room_link(fd, "cafeteria", "bad room!", 40);
	if (expect_stats(fd, 5, 1, 3, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_add_room_link(fd, "cafeteria", "admin", 40);
	if (expect_stats(fd, 6, 2, 3, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_route_closed(fd, "cafeteria", "admin");
	if (expect_stats(fd, 7, 3, 3, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_route_closed(fd, "cafeteria", "admin");
	if (expect_stats(fd, 8, 3, 3, 1, 0) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_observe_self(fd, 20, FD_PHASE_PLAYING, long_room, 0, 0, 0);
	if (expect_stats(fd, 9, 3, 3, 1, 1) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_reset(fd);
	if (expect_stats(fd, 9, 3, 3, 1, 1) != 0) {
		fd_destroy(fd);
		return 1;
	}

	fd_destroy(fd);
	puts("test_ingest_stats_contract: OK");
	return 0;
}
