# BSDmakefile - read by bmake/pmake before Makefile/makefile.
# Invoke:   make all         (OpenBSD, FreeBSD, NetBSD)
#           make test        run snapshot test
#           make clean
# The GNUmakefile is the entrypoint for gmake / Linux / macOS.
#
# OpenBSD: HAVE_PLEDGE / HAVE_UNVEIL gate the calls in src/driver.c.
# These are libc functions on OpenBSD; no extra link flag required.

.include "mk/sources.mk"

OS != uname -s

CC       ?= cc
AR       ?= ar
CSTD     ?= -std=c11
WARN     ?= -Wall -Wextra -Wpedantic
OPT      ?= -O2 -g
CFLAGS   += ${CSTD} ${WARN} ${OPT}
CPPFLAGS += -I${CLIPS_DIR} -I${SRC_DIR}
LDLIBS   ?= -lm
# All objects are PIC so one object tree serves the static archives and
# the shared library; the cost on x86-64/arm64 is negligible.
PICFLAG  ?= -fPIC
PREFIX   ?= /usr/local
DESTDIR  ?=

# Shared library (ELF; the macOS dylib case lives in the GNUmakefile).
# Only fd_* symbols are exported: the vendored CLIPS engine stays local.
SHLIB_FILE  = libfakedetector.so.${FD_ABI}
SHLIB_LINK  = libfakedetector.so
SHLIB_FLAGS = -shared -Wl,-soname,${SHLIB_FILE} \
              -Wl,--version-script=mk/fd_exports.map
SHLIB       = ${BUILD_DIR}/${SHLIB_FILE}

# pledge(2) and unveil(2) are OpenBSD-only. Enable the sandbox path solely
# there; on other BSDs (or Linux bmake) driver.c compiles without it.
#
# OpenBSD exposes POSIX.1-2008 (strdup, clock_gettime) in its default
# namespace, so APP_POSIX's -D_POSIX_C_SOURCE is unnecessary there -- and
# harmful: defining it clears __BSD_VISIBLE in <sys/cdefs.h>, which hides
# pledge(2)/unveil(2) from <unistd.h> and breaks the sandbox build. Clear
# APP_POSIX on OpenBSD; other platforms (incl. Linux via bmake) keep it.
.if ${OS} == "OpenBSD"
CPPFLAGS += -DHAVE_PLEDGE -DHAVE_UNVEIL
APP_POSIX =
.endif

# Mirror each source path into the build tree:
#   vendor/clips/core/agenda.c     -> build/clips/agenda.o
#   src/fakedetector.c             -> build/app/fakedetector.o
#   src/driver.c                   -> build/app/driver.o
CLIPS_OBJS  = ${CLIPS_SRCS:C|^${CLIPS_DIR}/|${BUILD_DIR}/clips/|:.c=.o}
LIB_OBJS    = ${LIB_SRCS:C|^${SRC_DIR}/|${BUILD_DIR}/app/|:.c=.o}
DRIVER_OBJS = ${DRIVER_SRCS:C|^${SRC_DIR}/|${BUILD_DIR}/app/|:.c=.o}
PERF_OBJS   = ${PERF_SRCS:C|^${SRC_DIR}/|${BUILD_DIR}/app/|:.c=.o}
APP_OBJS    = ${APP_SRCS:C|^${SRC_DIR}/|${BUILD_DIR}/app/|:.c=.o}
TEST_OBJS   = ${TEST_SRCS:C|^tests/|${BUILD_DIR}/tests/|:.c=.o}
TEST_BINS   = ${TEST_SRCS:C|^tests/|${BUILD_DIR}/tests/|:S/.c$//}

.PHONY: all clean test perf dirs test-sanitize shared install

all: dirs ${DRIVER_BIN}

${DRIVER_BIN}: ${DRIVER_OBJS} ${LIB_NAME} ${CLIPS_LIB}
	${CC} ${CFLAGS} ${LINK_SANITIZE} -o ${.TARGET} ${DRIVER_OBJS} ${LIB_NAME} ${CLIPS_LIB} ${LDLIBS}

${PERF_BIN}: ${PERF_OBJS} ${LIB_NAME} ${CLIPS_LIB}
	${CC} ${CFLAGS} ${LINK_SANITIZE} -o ${.TARGET} ${PERF_OBJS} ${LIB_NAME} ${CLIPS_LIB} ${LDLIBS}

${LIB_NAME}: ${LIB_OBJS} ${EMBED_OBJ}
	${AR} rcs ${.TARGET} ${LIB_OBJS} ${EMBED_OBJ}

${CLIPS_LIB}: ${CLIPS_OBJS}
	${AR} rcs ${.TARGET} ${CLIPS_OBJS}

# Embedded rules: regenerated whenever a rule file or the generator
# changes; compiled with the strict project flags (it is our code).
${EMBED_GEN}: ${CLP_FILES} mk/embed-clp.sh
	sh mk/embed-clp.sh ${.TARGET} ${CLP_FILES}

${EMBED_OBJ}: ${EMBED_GEN} ${SRC_DIR}/fakedetector.h ${SRC_DIR}/fd_internal.h
	${CC} ${CFLAGS} ${APP_STRICT} ${PICFLAG} ${CPPFLAGS} ${APP_POSIX} ${APP_SANITIZE} -c -o ${.TARGET} ${EMBED_GEN}

# Shared library: everything (fd + embedded rules + CLIPS) in one
# artifact, exporting only fd_*.
shared: dirs ${SHLIB}

${SHLIB}: ${LIB_OBJS} ${EMBED_OBJ} ${CLIPS_OBJS} mk/fd_exports.map
	${CC} ${SHLIB_FLAGS} -o ${.TARGET} ${LIB_OBJS} ${EMBED_OBJ} ${CLIPS_OBJS} ${LDLIBS}
	ln -sf ${SHLIB_FILE} ${BUILD_DIR}/${SHLIB_LINK}

# Merged static archive, install-only (see mk/sources.mk).
${FULL_LIB}: ${LIB_OBJS} ${EMBED_OBJ} ${CLIPS_OBJS}
	${AR} rcs ${.TARGET} ${LIB_OBJS} ${EMBED_OBJ} ${CLIPS_OBJS}

${BUILD_DIR}/fakedetector.pc: mk/fakedetector.pc.in
	sed -e 's|@PREFIX@|${PREFIX}|' -e 's|@VERSION@|${FD_VERSION}|' mk/fakedetector.pc.in > ${.TARGET}

install: dirs ${SHLIB} ${FULL_LIB} ${BUILD_DIR}/fakedetector.pc
	mkdir -p ${DESTDIR}${PREFIX}/include ${DESTDIR}${PREFIX}/lib/pkgconfig ${DESTDIR}${PREFIX}/share/fakedetector/clp
	install -m 644 ${SRC_DIR}/fakedetector.h ${SRC_DIR}/fd_crewrift.h ${DESTDIR}${PREFIX}/include/
	install -m 644 ${FULL_LIB} ${DESTDIR}${PREFIX}/lib/libfakedetector.a
	install -m 755 ${SHLIB} ${DESTDIR}${PREFIX}/lib/
	ln -sf ${SHLIB_FILE} ${DESTDIR}${PREFIX}/lib/${SHLIB_LINK}
	install -m 644 ${BUILD_DIR}/fakedetector.pc ${DESTDIR}${PREFIX}/lib/pkgconfig/
	install -m 644 ${CLP_FILES} ${DESTDIR}${PREFIX}/share/fakedetector/clp/

# Per-file compile rules generated at parse time. Path-mirror transform is
# repeated inside .for because bmake lacks GNU's %-pattern targets.
# Note: .IMPSRC is restricted to suffix/inference rules; these are explicit
# rules, so use .ALLSRC (the rule's only prereq is the source file).
# Vendored CLIPS: base flags only (no -Werror; it warns heavily).
.for src in ${CLIPS_SRCS}
${src:C|^${CLIPS_DIR}/|${BUILD_DIR}/clips/|:.c=.o}: ${src}
	${CC} ${CFLAGS} ${PICFLAG} ${CPPFLAGS} -c -o ${.TARGET} ${.ALLSRC}
.endfor

# Project sources: base flags plus the strict warning set and the
# POSIX feature macro. Every project object depends on both project
# headers — there is no automatic depfile tracking, and a stale object
# compiled against an old fd_detector layout silently corrupts struct
# offsets across the library. (Compile ${src}, not ${.ALLSRC}: the
# header prereqs must not land on the compile line.)
.for src in ${APP_SRCS}
${src:C|^${SRC_DIR}/|${BUILD_DIR}/app/|:.c=.o}: ${src} ${SRC_DIR}/fakedetector.h ${SRC_DIR}/fd_internal.h
	${CC} ${CFLAGS} ${APP_STRICT} ${PICFLAG} ${CPPFLAGS} ${APP_POSIX} ${APP_SANITIZE} -c -o ${.TARGET} ${src}
.endfor

.for src in ${TEST_SRCS}
${src:C|^tests/|${BUILD_DIR}/tests/|:.c=.o}: ${src} ${SRC_DIR}/fakedetector.h ${SRC_DIR}/fd_internal.h ${SRC_DIR}/fd_crewrift.h
	${CC} ${CFLAGS} ${APP_STRICT} ${CPPFLAGS} ${APP_POSIX} ${APP_SANITIZE} -c -o ${.TARGET} ${src}
${src:C|^tests/|${BUILD_DIR}/tests/|:S/.c$//}: ${src:C|^tests/|${BUILD_DIR}/tests/|:.c=.o} ${LIB_NAME} ${CLIPS_LIB}
	${CC} ${CFLAGS} ${LINK_SANITIZE} -o ${.TARGET} ${src:C|^tests/|${BUILD_DIR}/tests/|:.c=.o} ${LIB_NAME} ${CLIPS_LIB} ${LDLIBS}
.endfor

dirs:
	@mkdir -p ${BUILD_DIR}/clips ${BUILD_DIR}/app ${BUILD_DIR}/tests ${BUILD_DIR}/gen

# Snapshot test: run the driver, diff its stdout against the captured
# expected output. The driver also self-asserts API contracts and returns
# non-zero if a stance regresses.
test: dirs ${TEST_BINS} ${DRIVER_BIN}
	@for t in ${TEST_BINS}; do ./$$t || exit 1; done
	./${DRIVER_BIN} > ${TEST_ACTUAL}
	diff -u ${TEST_EXPECTED} ${TEST_ACTUAL}

# Performance test: run three elaborate scenarios (16 colors, multiple
# voting rounds, many safety checks, scenario C exceeds 1000 fact
# assertions) and fail if any public-API call exceeds its kind's
# budget — FD_PERF_ASSERT_BUDGET_NS (default 5 ms) for state-mutating
# calls, FD_PERF_QUERY_BUDGET_NS (default 10 ms) for queries. Not
# wired into `test` because per-call timing is environment-sensitive.
perf: dirs ${PERF_BIN}
	./${PERF_BIN}

test-sanitize:
	${MAKE} clean
	ASAN_OPTIONS=detect_leaks=0 ${MAKE} test \
	    APP_SANITIZE='-fsanitize=address,undefined -fno-omit-frame-pointer' \
	    LINK_SANITIZE='-fsanitize=address,undefined'
	${MAKE} clean

clean:
	rm -rf ${BUILD_DIR}
