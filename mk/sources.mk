# mk/sources.mk — shared variable definitions
# Plain VAR = value assignments; included by both BSDmakefile and GNUmakefile.
# Do not put dialect-specific syntax (conditionals, functions, suffix/pattern rules) here.

CLIPS_DIR  = vendor/clips/core
SRC_DIR    = src
BUILD_DIR  = build

CLIPS_LIB  = $(BUILD_DIR)/libclips.a

# Library + ABI version. FD_ABI names the shared library (soname) and
# must match FD_ABI_VERSION in fakedetector.h; FD_VERSION must match
# FD_VERSION_STRING. See docs/abi.md.
FD_VERSION = 1.0.0
FD_ABI     = 1

# Rule files, in canonical load order (mirrors fd_create). This list
# drives both the embedded-rules generation and the install target.
CLP_FILES  = clp/templates.clp \
             clp/dossier.clp \
             clp/cases.clp \
             clp/claims.clp \
             clp/social.clp \
             clp/suspicion.clp \
             clp/stance.clp

# Generated embedded-rules translation unit (mk/embed-clp.sh).
EMBED_GEN  = $(BUILD_DIR)/gen/fd_rules_embedded.c
EMBED_OBJ  = $(BUILD_DIR)/gen/fd_rules_embedded.o

# Single merged archive for `install`: fd objects + embedded rules +
# CLIPS, so an installed consumer links -lfakedetector -lm and nothing
# else. In-tree dev builds keep the two separate archives.
FULL_LIB   = $(BUILD_DIR)/libfakedetector-full.a

# fake-detector library + dev harness
# Library translation units. Each owns a single responsibility;
# fd_internal.h carries the cross-module struct + helper prototypes.
LIB_SRCS   = $(SRC_DIR)/fakedetector.c \
             $(SRC_DIR)/fd_schema.c \
             $(SRC_DIR)/fd_clips.c \
             $(SRC_DIR)/fd_graph.c \
             $(SRC_DIR)/fd_observe.c \
             $(SRC_DIR)/fd_query.c \
             $(SRC_DIR)/fd_render.c \
             $(SRC_DIR)/fd_telemetry.c \
             $(SRC_DIR)/fd_profile.c
LIB_NAME   = $(BUILD_DIR)/libfakedetector.a

DRIVER_SRCS = $(SRC_DIR)/driver.c
DRIVER_BIN  = $(BUILD_DIR)/driver

PERF_SRCS  = $(SRC_DIR)/perf.c
PERF_BIN   = $(BUILD_DIR)/perf

TEST_SRCS  = tests/test_suspicion_contract.c \
             tests/test_query_run_contract.c \
             tests/test_ingest_stats_contract.c \
             tests/test_schema_validation_contract.c \
             tests/test_embedded_rules_contract.c \
             tests/test_player_name_contract.c
TEST_OBJS  = $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS  = $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

# APP_* is the union the path-mirror :C / patsubst transforms apply to in
# both makefiles. APP_BIN is the primary `all` target.
APP_SRCS   = $(LIB_SRCS) $(DRIVER_SRCS) $(PERF_SRCS)
APP_BIN    = $(DRIVER_BIN)

# Extra-strict flags for the project's own sources only. Vendored CLIPS
# is warning-noisy and must not be modified, so it is built without
# these: the project code can be -Werror-clean without drowning in
# third-party noise. APP_POSIX opts project code into POSIX.1-2008
# (strdup, clock_gettime) consistently rather than per-file macros.
APP_STRICT = -Werror -Wmissing-prototypes -Wstrict-prototypes \
             -Wshadow -Wpointer-arith
APP_POSIX  = -D_POSIX_C_SOURCE=200809L
APP_SANITIZE  ?=
LINK_SANITIZE ?=

# Snapshot test
TEST_EXPECTED = tests/expected/driver.out
TEST_ACTUAL   = $(BUILD_DIR)/driver.out

# CLIPS engine source list — 166 files (vendor/clips/core/*.c minus main.c).
# Generated once after vendoring: ls vendor/clips/core/*.c | grep -v '/main\.c$' | sort
CLIPS_SRCS = \
  vendor/clips/core/agenda.c \
  vendor/clips/core/analysis.c \
  vendor/clips/core/argacces.c \
  vendor/clips/core/bload.c \
  vendor/clips/core/bmathfun.c \
  vendor/clips/core/bsave.c \
  vendor/clips/core/classcom.c \
  vendor/clips/core/classexm.c \
  vendor/clips/core/classfun.c \
  vendor/clips/core/classinf.c \
  vendor/clips/core/classini.c \
  vendor/clips/core/classpsr.c \
  vendor/clips/core/clsltpsr.c \
  vendor/clips/core/commline.c \
  vendor/clips/core/conscomp.c \
  vendor/clips/core/constrct.c \
  vendor/clips/core/constrnt.c \
  vendor/clips/core/crstrtgy.c \
  vendor/clips/core/cstrcbin.c \
  vendor/clips/core/cstrccom.c \
  vendor/clips/core/cstrcpsr.c \
  vendor/clips/core/cstrnbin.c \
  vendor/clips/core/cstrnchk.c \
  vendor/clips/core/cstrncmp.c \
  vendor/clips/core/cstrnops.c \
  vendor/clips/core/cstrnpsr.c \
  vendor/clips/core/cstrnutl.c \
  vendor/clips/core/default.c \
  vendor/clips/core/defins.c \
  vendor/clips/core/developr.c \
  vendor/clips/core/dffctbin.c \
  vendor/clips/core/dffctbsc.c \
  vendor/clips/core/dffctcmp.c \
  vendor/clips/core/dffctdef.c \
  vendor/clips/core/dffctpsr.c \
  vendor/clips/core/dffnxbin.c \
  vendor/clips/core/dffnxcmp.c \
  vendor/clips/core/dffnxexe.c \
  vendor/clips/core/dffnxfun.c \
  vendor/clips/core/dffnxpsr.c \
  vendor/clips/core/dfinsbin.c \
  vendor/clips/core/dfinscmp.c \
  vendor/clips/core/drive.c \
  vendor/clips/core/emathfun.c \
  vendor/clips/core/engine.c \
  vendor/clips/core/envrnbld.c \
  vendor/clips/core/envrnmnt.c \
  vendor/clips/core/evaluatn.c \
  vendor/clips/core/expressn.c \
  vendor/clips/core/exprnbin.c \
  vendor/clips/core/exprnops.c \
  vendor/clips/core/exprnpsr.c \
  vendor/clips/core/extnfunc.c \
  vendor/clips/core/factbin.c \
  vendor/clips/core/factbld.c \
  vendor/clips/core/factcmp.c \
  vendor/clips/core/factcom.c \
  vendor/clips/core/factfile.c \
  vendor/clips/core/factfun.c \
  vendor/clips/core/factgen.c \
  vendor/clips/core/facthsh.c \
  vendor/clips/core/factlhs.c \
  vendor/clips/core/factmch.c \
  vendor/clips/core/factmngr.c \
  vendor/clips/core/factprt.c \
  vendor/clips/core/factqpsr.c \
  vendor/clips/core/factqury.c \
  vendor/clips/core/factrete.c \
  vendor/clips/core/factrhs.c \
  vendor/clips/core/filecom.c \
  vendor/clips/core/filertr.c \
  vendor/clips/core/fileutil.c \
  vendor/clips/core/generate.c \
  vendor/clips/core/genrcbin.c \
  vendor/clips/core/genrccmp.c \
  vendor/clips/core/genrccom.c \
  vendor/clips/core/genrcexe.c \
  vendor/clips/core/genrcfun.c \
  vendor/clips/core/genrcpsr.c \
  vendor/clips/core/globlbin.c \
  vendor/clips/core/globlbsc.c \
  vendor/clips/core/globlcmp.c \
  vendor/clips/core/globlcom.c \
  vendor/clips/core/globldef.c \
  vendor/clips/core/globlpsr.c \
  vendor/clips/core/immthpsr.c \
  vendor/clips/core/incrrset.c \
  vendor/clips/core/inherpsr.c \
  vendor/clips/core/inscom.c \
  vendor/clips/core/insfile.c \
  vendor/clips/core/insfun.c \
  vendor/clips/core/insmngr.c \
  vendor/clips/core/insmoddp.c \
  vendor/clips/core/insmult.c \
  vendor/clips/core/inspsr.c \
  vendor/clips/core/insquery.c \
  vendor/clips/core/insqypsr.c \
  vendor/clips/core/iofun.c \
  vendor/clips/core/lgcldpnd.c \
  vendor/clips/core/memalloc.c \
  vendor/clips/core/miscfun.c \
  vendor/clips/core/modulbin.c \
  vendor/clips/core/modulbsc.c \
  vendor/clips/core/modulcmp.c \
  vendor/clips/core/moduldef.c \
  vendor/clips/core/modulpsr.c \
  vendor/clips/core/modulutl.c \
  vendor/clips/core/msgcom.c \
  vendor/clips/core/msgfun.c \
  vendor/clips/core/msgpass.c \
  vendor/clips/core/msgpsr.c \
  vendor/clips/core/multifld.c \
  vendor/clips/core/multifun.c \
  vendor/clips/core/objbin.c \
  vendor/clips/core/objcmp.c \
  vendor/clips/core/objrtbin.c \
  vendor/clips/core/objrtbld.c \
  vendor/clips/core/objrtcmp.c \
  vendor/clips/core/objrtfnx.c \
  vendor/clips/core/objrtgen.c \
  vendor/clips/core/objrtmch.c \
  vendor/clips/core/parsefun.c \
  vendor/clips/core/pattern.c \
  vendor/clips/core/pprint.c \
  vendor/clips/core/prccode.c \
  vendor/clips/core/prcdrfun.c \
  vendor/clips/core/prcdrpsr.c \
  vendor/clips/core/prdctfun.c \
  vendor/clips/core/prntutil.c \
  vendor/clips/core/proflfun.c \
  vendor/clips/core/reorder.c \
  vendor/clips/core/reteutil.c \
  vendor/clips/core/retract.c \
  vendor/clips/core/router.c \
  vendor/clips/core/rulebin.c \
  vendor/clips/core/rulebld.c \
  vendor/clips/core/rulebsc.c \
  vendor/clips/core/rulecmp.c \
  vendor/clips/core/rulecom.c \
  vendor/clips/core/rulecstr.c \
  vendor/clips/core/ruledef.c \
  vendor/clips/core/ruledlt.c \
  vendor/clips/core/rulelhs.c \
  vendor/clips/core/rulepsr.c \
  vendor/clips/core/scanner.c \
  vendor/clips/core/sortfun.c \
  vendor/clips/core/strngfun.c \
  vendor/clips/core/strngrtr.c \
  vendor/clips/core/symblbin.c \
  vendor/clips/core/symblcmp.c \
  vendor/clips/core/symbol.c \
  vendor/clips/core/sysdep.c \
  vendor/clips/core/textpro.c \
  vendor/clips/core/tmpltbin.c \
  vendor/clips/core/tmpltbsc.c \
  vendor/clips/core/tmpltcmp.c \
  vendor/clips/core/tmpltdef.c \
  vendor/clips/core/tmpltfun.c \
  vendor/clips/core/tmpltlhs.c \
  vendor/clips/core/tmpltpsr.c \
  vendor/clips/core/tmpltrhs.c \
  vendor/clips/core/tmpltutl.c \
  vendor/clips/core/userdata.c \
  vendor/clips/core/userfunctions.c \
  vendor/clips/core/utility.c \
  vendor/clips/core/watch.c
