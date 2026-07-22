/*
 * Contract: fd_create(NULL) loads the build-time-embedded rule set and
 * behaves identically to fd_create("clp") loading the same files from
 * disk — proven by feeding both detectors one round and comparing their
 * full working-memory snapshots byte for byte. Also covers the
 * FFI-facing telemetry variants (fd_dump_state_buf snprintf semantics,
 * fd_dump_state_path) and the version handshake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fd_internal.h"

#define DUMP_CAP 65536

/* Test-local player ids (legacy roster order); default "p<id>" names. */
enum { P_RED = 0, P_YELLOW = 2, P_PINK = 4, P_BLUE = 6 };

static int
fail(const char *msg)
{
	fprintf(stderr, "test_embedded_rules_contract: %s\n", msg);
	return 1;
}

static void
feed_round(fd_detector *fd)
{
	fd_add_room_link(fd, "cafeteria", "admin", 50);
	fd_add_room_link(fd, "cafeteria", "medbay", 40);
	fd_add_room_link(fd, "medbay", "storage", 50);

	fd_set_self(fd, P_RED);
	fd_observe_round_config(fd, 5, 1);
	fd_observe_self(fd, 0, FD_PHASE_PLAYING, "cafeteria", 0, 0, 0);
	fd_observe_player(fd, 10, P_BLUE, "cafeteria", 0, 0);
	fd_observe_player(fd, 10, P_PINK, "cafeteria", 0, 0);
	fd_observe_player(fd, 40, P_BLUE, "cafeteria", 0, 0);
	fd_observe_task_completion(fd, 40, P_BLUE, "cafeteria");
	fd_observe_player(fd, 90, P_PINK, "storage", 0, 0);
	fd_observe_body(fd, 120, P_YELLOW, "storage", 0, 0);
	fd_run(fd);
}

int
main(void)
{
	if (strcmp(fd_version(), FD_VERSION_STRING) != 0)
		return fail("fd_version() != FD_VERSION_STRING");
	if (fd_abi_version() != FD_ABI_VERSION)
		return fail("fd_abi_version() != FD_ABI_VERSION");

	fd_detector *emb = fd_create(NULL);
	if (emb == NULL)
		return fail("fd_create(NULL) [embedded rules] failed");
	fd_detector *dsk = fd_create("clp");
	if (dsk == NULL) {
		fd_destroy(emb);
		return fail("fd_create(\"clp\") failed");
	}

	feed_round(emb);
	feed_round(dsk);

	static char dump_emb[DUMP_CAP], dump_dsk[DUMP_CAP];
	long len_emb = fd_dump_state_buf(emb, dump_emb, sizeof dump_emb);
	long len_dsk = fd_dump_state_buf(dsk, dump_dsk, sizeof dump_dsk);
	int rc = 1;

	if (len_emb <= 0 || len_emb >= (long)sizeof dump_emb) {
		fail("embedded dump empty or truncated");
	} else if (len_emb != len_dsk || strcmp(dump_emb, dump_dsk) != 0) {
		fail("embedded and disk rule sets produced different snapshots");
	} else if (fd_dump_state_buf(emb, NULL, 0) != len_emb) {
		fail("size query (NULL, 0) != full dump length");
	} else {
		/* Truncation keeps snprintf semantics: full length returned,
		 * buffer NUL-terminated within cap. */
		char tiny[16];
		long len_tiny = fd_dump_state_buf(emb, tiny, sizeof tiny);
		if (len_tiny != len_emb) {
			fail("truncated dump did not report full length");
		} else if (tiny[sizeof tiny - 1] != '\0' ||
		    strncmp(tiny, dump_emb, sizeof tiny - 1) != 0) {
			fail("truncated dump content mismatch");
		} else {
			char path[256];
			snprintf(path, sizeof path, "%s/fd-embed-test-%d.dump",
			    getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
			    (int)0);
			if (fd_dump_state_path(emb, path) != 0) {
				fail("fd_dump_state_path failed");
			} else {
				FILE *f = fopen(path, "r");
				static char file_dump[DUMP_CAP];
				size_t n = f ? fread(file_dump, 1,
				    sizeof file_dump - 1, f) : 0;
				if (f != NULL)
					fclose(f);
				remove(path);
				file_dump[n] = '\0';
				if ((long)n != len_emb ||
				    strcmp(file_dump, dump_emb) != 0)
					fail("path dump != buffer dump");
				else
					rc = 0;
			}
		}
	}

	fd_destroy(emb);
	fd_destroy(dsk);
	if (rc == 0)
		puts("test_embedded_rules_contract: OK");
	return rc;
}
