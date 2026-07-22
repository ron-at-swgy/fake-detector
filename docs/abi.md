# Versioning and ABI policy

The library carries two version identities, declared in `src/fakedetector.h`
and mirrored by `FD_VERSION` / `FD_ABI` in `mk/sources.mk` (keep all four in
sync — the makefile values name the artifacts, the macros are the compile-time
contract):

- **`FD_VERSION_*` / `fd_version()`** — the semantic version of the library
  (`MAJOR.MINOR.PATCH`).
- **`FD_ABI_VERSION` / `fd_abi_version()`** — a single integer bumped on any
  ABI-incompatible change. The shared library's soname tracks it
  (`libfakedetector.so.1`).

The macros describe the header a consumer compiled against; the functions
report the library actually linked or `dlopen`ed. **Bindings must call
`fd_abi_version()` at load time and refuse to proceed on a mismatch** — a
Python or Nim wrapper mirrors struct layouts by hand, and a silent mismatch
corrupts memory instead of failing cleanly.

## Compatibility rules

Pre-1.0, a minor bump may break anything.

From 1.0.0 on:

- **Frozen layout**: the public out-structs (`fd_vote_decision_t`,
  `fd_suspicion_explain`, `fd_run_stats`, `fd_ingest_stats`,
  `fd_round_stats`) never change size or field order within a major
  version.
- **Append-only enums**: new enumerators are added after the last existing
  one; existing values are never renumbered.
- **Additive API**: new functions may appear in a minor release; signatures
  of existing functions only change with a major release and an
  `FD_ABI_VERSION` bump.
- **Soname**: `FD_ABI_VERSION` bumps exactly when a compiled-and-linked
  consumer (or a hand-mirrored binding) would misbehave without
  recompiling/updating. Pure additions do not bump it.

## What is NOT stable surface

`fd_profile_*` (diagnostic wrappers, marked in the header), the textual
content of rationale strings (the *fields* are stable, the prose may be
tuned), and the exact per-mille suspicion values (the *distribution
contract* — 0..1000 summing to 1000 over the alive pool — is stable, the
increments in `clp/suspicion.clp` are tunable).

## Rule files and the embedded copy

`fd_create(NULL)` loads the rule set embedded at build time; passing a
directory loads from disk. The embedded copy and `clp/` are the same files
by construction (`mk/embed-clp.sh` runs at build), so a given library binary
pins its rule semantics. Consumers that ship modified rules load them from
disk and own the resulting behavior.
