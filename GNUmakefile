# GNUmakefile - read by gmake before Makefile/makefile.
# Invoke:   gmake all        (Linux, macOS)
#           gmake test       run snapshot test
#           gmake clean
# The BSDmakefile is the entrypoint for bmake / OpenBSD.

include mk/sources.mk

CC      ?= cc
AR      ?= ar
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
OPT     ?= -O2 -g
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)
CPPFLAGS ?= -I$(CLIPS_DIR) -I$(SRC_DIR)
LDLIBS  ?= -lm
# All objects are PIC so one object tree serves the static archives and
# the shared library; the cost on x86-64/arm64 is negligible.
PICFLAG ?= -fPIC
PREFIX  ?= /usr/local
DESTDIR ?=

# Shared-library naming and link flags per platform. Only fd_* symbols
# are exported (mk/fd_exports.*): the vendored CLIPS engine stays local.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHLIB_FILE  = libfakedetector.$(FD_ABI).dylib
SHLIB_LINK  = libfakedetector.dylib
SHLIB_FLAGS = -dynamiclib -install_name @rpath/$(SHLIB_FILE) \
              -Wl,-exported_symbols_list,mk/fd_exports.macos
SHLIB_DEPS  = mk/fd_exports.macos
else
SHLIB_FILE  = libfakedetector.so.$(FD_ABI)
SHLIB_LINK  = libfakedetector.so
SHLIB_FLAGS = -shared -Wl,-soname,$(SHLIB_FILE) \
              -Wl,--version-script=mk/fd_exports.map
SHLIB_DEPS  = mk/fd_exports.map
endif
SHLIB = $(BUILD_DIR)/$(SHLIB_FILE)

CLIPS_OBJS  := $(patsubst $(CLIPS_DIR)/%.c,$(BUILD_DIR)/clips/%.o,$(CLIPS_SRCS))
LIB_OBJS    := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/app/%.o,$(LIB_SRCS))
DRIVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/app/%.o,$(DRIVER_SRCS))
PERF_OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/app/%.o,$(PERF_SRCS))
TEST_OBJS   := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS   := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

.PHONY: all clean test perf test-sanitize shared install
all: $(APP_BIN)

$(DRIVER_BIN): $(DRIVER_OBJS) $(LIB_NAME) $(CLIPS_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LINK_SANITIZE) -o $@ $(DRIVER_OBJS) $(LIB_NAME) $(CLIPS_LIB) $(LDLIBS)

$(PERF_BIN): $(PERF_OBJS) $(LIB_NAME) $(CLIPS_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LINK_SANITIZE) -o $@ $(PERF_OBJS) $(LIB_NAME) $(CLIPS_LIB) $(LDLIBS)

$(BUILD_DIR)/tests/%: tests/%.c $(SRC_DIR)/fakedetector.h $(SRC_DIR)/fd_internal.h $(SRC_DIR)/fd_crewrift.h $(LIB_NAME) $(CLIPS_LIB) | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) $(APP_STRICT) $(CPPFLAGS) $(APP_POSIX) $(APP_SANITIZE) \
	    -o $@ $< $(LIB_NAME) $(CLIPS_LIB) $(LINK_SANITIZE) $(LDLIBS)

$(LIB_NAME): $(LIB_OBJS) $(EMBED_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIB_OBJS) $(EMBED_OBJ)

$(CLIPS_LIB): $(CLIPS_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $(CLIPS_OBJS)

# Vendored CLIPS: base flags only (no -Werror; it warns heavily).
$(BUILD_DIR)/clips/%.o: $(CLIPS_DIR)/%.c | $(BUILD_DIR)/clips
	$(CC) $(CFLAGS) $(PICFLAG) $(CPPFLAGS) -c -o $@ $<

# Project sources: base flags plus the strict warning set and the
# POSIX feature macro. Every project object depends on both project
# headers — there is no automatic depfile tracking, and a stale object
# compiled against an old fd_detector layout silently corrupts struct
# offsets across the library.
$(BUILD_DIR)/app/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/fakedetector.h $(SRC_DIR)/fd_internal.h | $(BUILD_DIR)/app
	$(CC) $(CFLAGS) $(APP_STRICT) $(PICFLAG) $(CPPFLAGS) $(APP_POSIX) $(APP_SANITIZE) \
	    -c -o $@ $<

# Embedded rules: regenerated whenever a rule file or the generator
# changes; compiled with the strict project flags (it is our code).
$(EMBED_GEN): $(CLP_FILES) mk/embed-clp.sh | $(BUILD_DIR)/gen
	sh mk/embed-clp.sh $@ $(CLP_FILES)

$(EMBED_OBJ): $(EMBED_GEN) $(SRC_DIR)/fakedetector.h $(SRC_DIR)/fd_internal.h
	$(CC) $(CFLAGS) $(APP_STRICT) $(PICFLAG) $(CPPFLAGS) $(APP_POSIX) $(APP_SANITIZE) \
	    -c -o $@ $(EMBED_GEN)

# Shared library: everything (fd + embedded rules + CLIPS) in one
# artifact, exporting only fd_*.
shared: $(SHLIB)

$(SHLIB): $(LIB_OBJS) $(EMBED_OBJ) $(CLIPS_OBJS) $(SHLIB_DEPS) | $(BUILD_DIR)
	$(CC) $(SHLIB_FLAGS) -o $@ $(LIB_OBJS) $(EMBED_OBJ) $(CLIPS_OBJS) $(LDLIBS)
	ln -sf $(SHLIB_FILE) $(BUILD_DIR)/$(SHLIB_LINK)

# Merged static archive, install-only (see mk/sources.mk).
$(FULL_LIB): $(LIB_OBJS) $(EMBED_OBJ) $(CLIPS_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIB_OBJS) $(EMBED_OBJ) $(CLIPS_OBJS)

$(BUILD_DIR)/fakedetector.pc: mk/fakedetector.pc.in | $(BUILD_DIR)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@VERSION@|$(FD_VERSION)|' $< > $@

install: $(SHLIB) $(FULL_LIB) $(BUILD_DIR)/fakedetector.pc
	mkdir -p $(DESTDIR)$(PREFIX)/include \
	    $(DESTDIR)$(PREFIX)/lib/pkgconfig \
	    $(DESTDIR)$(PREFIX)/share/fakedetector/clp
	install -m 644 $(SRC_DIR)/fakedetector.h $(SRC_DIR)/fd_crewrift.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 $(FULL_LIB) $(DESTDIR)$(PREFIX)/lib/libfakedetector.a
	install -m 755 $(SHLIB) $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(SHLIB_FILE) $(DESTDIR)$(PREFIX)/lib/$(SHLIB_LINK)
	install -m 644 $(BUILD_DIR)/fakedetector.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -m 644 $(CLP_FILES) $(DESTDIR)$(PREFIX)/share/fakedetector/clp/

$(BUILD_DIR) $(BUILD_DIR)/clips $(BUILD_DIR)/app $(BUILD_DIR)/tests $(BUILD_DIR)/gen:
	mkdir -p $@

# Snapshot test: run the driver, diff its stdout against the captured
# expected output. The driver also self-asserts API contracts and returns
# non-zero if a stance regresses.
test: $(TEST_BINS) $(APP_BIN)
	for t in $(TEST_BINS); do ./$$t || exit 1; done
	./$(APP_BIN) > $(TEST_ACTUAL)
	diff -u $(TEST_EXPECTED) $(TEST_ACTUAL)

# Performance test: run three elaborate scenarios (16 colors, multiple
# voting rounds, many safety checks, scenario C exceeds 1000 fact
# assertions) and fail if any public-API call exceeds its kind's
# budget — FD_PERF_ASSERT_BUDGET_NS (default 5 ms) for state-mutating
# calls, FD_PERF_QUERY_BUDGET_NS (default 10 ms) for queries. Not
# wired into `test` because per-call timing is environment-sensitive.
perf: $(PERF_BIN)
	./$(PERF_BIN)

test-sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) test \
	    APP_SANITIZE='-fsanitize=address,undefined -fno-omit-frame-pointer' \
	    LINK_SANITIZE='-fsanitize=address,undefined'
	$(MAKE) clean

clean:
	rm -rf $(BUILD_DIR)
