#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fakedetector.h"

static const char *const CLP_FILES[] = {
	"templates.clp",
	"dossier.clp",
	"cases.clp",
	"claims.clp",
	"social.clp",
	"suspicion.clp",
	"stance.clp"
};

static int
fail(const char *msg)
{
	fprintf(stderr, "test_schema_validation_contract: %s\n", msg);
	return 1;
}

static int
copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	FILE *out;
	char buf[4096];
	size_t n;

	if (in == NULL)
		return -1;
	out = fopen(dst, "wb");
	if (out == NULL) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -1;
		}
	}
	if (ferror(in)) {
		fclose(in);
		fclose(out);
		return -1;
	}
	fclose(in);
	fclose(out);
	return 0;
}

static int
rewrite_templates(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *buf;
	char *hit;
	long size;

	if (fp == NULL)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return -1;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(buf);
		return -1;
	}
	buf[size] = '\0';
	fclose(fp);

	hit = strstr(buf, "visible-tasks");
	if (hit == NULL) {
		free(buf);
		return -1;
	}
	memcpy(hit, "visible_tasks", strlen("visible_tasks"));

	fp = fopen(path, "wb");
	if (fp == NULL) {
		free(buf);
		return -1;
	}
	if (fwrite(buf, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(buf);
		return -1;
	}
	fclose(fp);
	free(buf);
	return 0;
}

static void
cleanup_dir(const char *dir)
{
	char path[512];

	for (size_t i = 0; i < sizeof CLP_FILES / sizeof CLP_FILES[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, CLP_FILES[i]);
		(void)unlink(path);
	}
	(void)rmdir(dir);
}

int
main(void)
{
	char dir_template[] = "/tmp/fd-schema-XXXXXX";
	char *tmpdir = mkdtemp(dir_template);
	char src[512];
	char dst[512];
	fd_detector *fd;

	if (tmpdir == NULL)
		return fail("mkdtemp failed");
	for (size_t i = 0; i < sizeof CLP_FILES / sizeof CLP_FILES[0]; i++) {
		snprintf(src, sizeof src, "clp/%s", CLP_FILES[i]);
		snprintf(dst, sizeof dst, "%s/%s", tmpdir, CLP_FILES[i]);
		if (copy_file(src, dst) != 0) {
			cleanup_dir(tmpdir);
			return fail("copy_file failed");
		}
	}

	fd = fd_create(tmpdir);
	if (fd == NULL) {
		cleanup_dir(tmpdir);
		return fail("copied CLP directory was not loadable");
	}
	fd_destroy(fd);

	snprintf(dst, sizeof dst, "%s/templates.clp", tmpdir);
	if (rewrite_templates(dst) != 0) {
		cleanup_dir(tmpdir);
		return fail("failed to corrupt templates.clp");
	}

	fd = fd_create(tmpdir);
	if (fd != NULL) {
		fd_destroy(fd);
		cleanup_dir(tmpdir);
		return fail("schema validation did not reject broken template slots");
	}

	cleanup_dir(tmpdir);
	puts("test_schema_validation_contract: OK");
	return 0;
}
