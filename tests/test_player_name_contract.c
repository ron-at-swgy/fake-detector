/*
 * Contract: the player-name registry. Registered names become the CLIPS
 * symbols and the words in rendered prose; invalid and duplicate names
 * are rejected (and counted as validation drops); names persist across
 * fd_reset; unnamed ids fall back to "p<id>"; fd_crewrift_register
 * installs the CrewRift roster.
 */

#include <stdio.h>
#include <string.h>

#include "fd_crewrift.h"

static int
fail(const char *msg)
{
	fprintf(stderr, "test_player_name_contract: %s\n", msg);
	return 1;
}

int
main(void)
{
	fd_detector *fd = fd_create(NULL);
	if (fd == NULL)
		return fail("fd_create(NULL) failed");
	int rc = 1;

	fd_ingest_stats before, after;

	if (strcmp(fd_player_name(fd, 0), "p0") != 0 ||
	    strcmp(fd_player_name(fd, 31), "p31") != 0) {
		fail("default names are not p<id>");
	} else if (fd_player_name(fd, -1) != NULL ||
	    fd_player_name(fd, FD_MAX_PLAYERS) != NULL) {
		fail("out-of-range id did not return NULL");
	} else if (fd_set_player_name(fd, 0, "detective_1") != 0) {
		fail("valid name rejected");
	} else if (strcmp(fd_player_name(fd, 0), "detective_1") != 0) {
		fail("registered name not returned");
	} else {
		fd_ingest_stats_get(fd, &before);
		int bad = 0;
		bad += fd_set_player_name(fd, 1, "no spaces") == 0;
		bad += fd_set_player_name(fd, 1, "") == 0;
		bad += fd_set_player_name(fd, 1, NULL) == 0;
		bad += fd_set_player_name(fd, -1, "ok") == 0;
		bad += fd_set_player_name(fd, FD_MAX_PLAYERS, "ok") == 0;
		bad += fd_set_player_name(fd, 1, "detective_1") == 0; /* dup */
		fd_ingest_stats_get(fd, &after);
		if (bad != 0) {
			fail("an invalid registration was accepted");
		} else if (after.validation_drops - before.validation_drops != 6) {
			fail("rejections not counted as validation drops");
		} else {
			/* Names survive reset; facts use them. */
			fd_reset(fd);
			if (strcmp(fd_player_name(fd, 0), "detective_1") != 0) {
				fail("name did not persist across fd_reset");
			} else {
				fd_set_player_name(fd, 2, "watson");
				fd_set_self(fd, 0);
				fd_observe_self(fd, 0, FD_PHASE_PLAYING,
				    "study", 0, 0, 0);
				fd_observe_player(fd, 10, 2, "study", 0, 0);
				fd_run(fd);
				/* Observed players appear in the dump under
				 * their registered symbol; self (never
				 * dossiered) appears in rendered prose. */
				static char dump[65536];
				char prose[1024];
				fd_dump_state_buf(fd, dump, sizeof dump);
				fd_render_vote_summary(fd, prose, sizeof prose);
				if (strstr(dump, "'watson'") == NULL ||
				    strstr(prose, "detective_1") == NULL) {
					fail("registered names absent from "
					    "dump / rendered prose");
				} else if (fd_crewrift_register(fd) != 0) {
					/* ids 0 and 2 are re-registered to
					 * CrewRift colors; the rest are
					 * fresh. All must succeed. */
					fail("fd_crewrift_register failed");
				} else if (strcmp(fd_player_name(fd, 0), "red") != 0 ||
				    strcmp(fd_player_name(fd, 15), "gray") != 0) {
					fail("CrewRift roster not applied");
				} else {
					rc = 0;
				}
			}
		}
	}

	fd_destroy(fd);
	if (rc == 0)
		puts("test_player_name_contract: OK");
	return rc;
}
