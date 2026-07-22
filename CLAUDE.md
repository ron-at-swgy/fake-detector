# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status

Library `libfakedetector.a` plus dev harness `build/driver` are in place; rules engine is wired end-to-end and exercised on a synthetic observation stream. Game-protocol integration is **not** in this repo — the library is designed to be linked into an Among Them player policy elsewhere.

## Purpose

The program watches crewmember actions in the **Among Them** game (an Among Us clone) and decides, for each other player, whether they are doing their job, doing it poorly, or behaving as an active threat. At voting time it emits a short natural-language justification suitable for the game's vote-chat slot.

Game-model grounding (verified against the game's observation encoder the first time we wrote rules): per-tick observations are structured (no name-by-name sabotage events), so threat evidence is largely circumstantial. Player identity is a caller-chosen integer id (`fd_player`, 0..`FD_MAX_PLAYERS`-1) with an optional registered display name — color-keyed games map their color index straight onto it (`src/fd_crewrift.h` ships the CrewRift roster). See `docs/model.md` for the fact schema rationale.

## Tech stack

- **C11** is the implementation language. Baseline flags: `-std=c11 -Wall -Wextra -Wpedantic`. CLIPS itself emits many `-Wunused-parameter` warnings; we do **not** use `-Werror` so the build is not gated on upstream noise.
- **CLIPS 6.4.2** is vendored at `vendor/clips/core/` (167 `.c` + 174 `.h`). We compile the engine into `build/libclips.a` from our own makefiles — the upstream `makefile` / `makefile.win` are kept for reference but not invoked. `main.c` is excluded so we own `main()`.
- **OpenBSD** is a primary deployment target. `pledge(2)` / `unveil(2)` are called from `src/driver.c` (and any consumer that links the library) when compiled with `-DHAVE_PLEDGE -DHAVE_UNVEIL`. Library code itself never calls pledge — that is the application's responsibility.

## Architectural shape

```
caller                          library                    rules / facts
------                          -------                    -------------
fd_set_self(self)         -->   asserts (self-color)
fd_observe_round_config   -->   asserts (roster)           round model
fd_observe_self(...)      -->   asserts (self-state)
fd_observe_player(...)    -->   asserts (sighting)         dossier rules
fd_observe_body(color,..) -->   asserts (body-seen)        -> crewmember
fd_observe_task(...)      -->   asserts (task-tick)        evidence rules
fd_observe_death(c)       -->   asserts (player-dead)      -> near-body
fd_observe_ejection(...)  -->   asserts (ejection)         status rules
                                                           -> player-dead, ejected
fd_observe_vent(...)      -->   asserts (vent-suspected)   case / claims / social
fd_observe_claim(...)     -->   asserts (claim)            -> contradiction,
fd_observe_vote(...)      -->   asserts (accuses)             corroboration, accuses
fd_observe_defense(...)   -->   asserts (defends)          suspicion rules
fd_run()                  -->   Run(env, -1)               -> roster, game-pressure,
                                + normalize (suspicion)       logodds-term -> suspicion
                                                           stance rules
                                                           -> stance (alive only)

fd_render_vote_summary    --    walks (stance) + status <-- stance.rationale
fd_render_case            --    walks case + alibi + suspect <-- (Poirot reveal)
fd_vote_decision          --    expected value over suspicion <-- (belief-aware vote)
fd_pick_vote_target       --    highest alive stance    <-- (stance-tier vote API)
fd_alone_risk(color)      --    stance + roster         <-- (safety query, gameplay)
fd_get_stance(color)      --    reads category          <--
fd_get_evidence(color)    --    reads stance.evidence   <-- (threat-source kind)
fd_get_suspicion(color)   --    reads suspicion.weight  <-- (calibrated 0..1000)
fd_explain_suspicion(c)   --    walks logodds-term       <-- (per-term attribution)
fd_get_status(color)      --    reads crewmember.status <--
fd_game_pressure()        --    reads game-pressure     <-- (parity vote urgency)
fd_win_probability()      --    w(n,m) from roster      <-- (instrumentation)
fd_run_stats_get()        --    reads run counters         (engine telemetry)
fd_dump_state(stream)     --    walks working memory       (state snapshot)
fd_trace_begin/end()      --    STDOUT-capture router      (rule-firing trace)
fd_room_distance(a,b)     --    graph shortest path        (graph sanity-check)
fd_reset()                ->    Reset(env)                 (between rounds)
```

Three architectural seams worth keeping clean:
1. **C side never decides who is fake.** Stance is always a CLIPS-derived fact; C just reads `stance` slots. The one sanctioned exception is arithmetic, not judgement: `fd_run` runs `fd_normalize_suspicion`, a C pass that divides the per-color `logodds-term` accumulators the rules produced into normalized `suspicion.weight` values. CLIPS still decides *who* is suspicious and *by how much* (in log space); C only performs the division — the same split as `room-distance` (C computes shortest paths, publishes them as facts).
2. **CLIPS never formats player-facing text.** Rules write a *rationale* string into the `stance` fact; C wraps it in the final prose.
3. **Library is observation-cadence-agnostic.** It does not own a clock or a socket. The caller asserts when convenient, calls `fd_run` when it wants rules to fire.

**Three orthogonal per-color axes.** `fd_player_status` (alive / dead / ejected) is about whether the player is still a vote target; `fd_stance` (on-task / off-task / threat / unknown) is the *categorical* read of how suspicious an *alive* player looks; `fd_get_suspicion` is the *magnitude* — a normalized per-mille belief weight (0..1000, summing to 1000 across the alive pool) so the policy can threshold and rank rather than match labels. Dead/ejected colors have no stance and no suspicion fact — they're filtered before any verdict logic runs, and their belief mass redistributes over the survivors.

**Vote-time APIs.** `fd_pick_vote_target` picks by stance tier + playstyle (legacy, deterministic). `fd_vote_decision` is the belief-aware picker: an expected-value test over the calibrated `suspicion` distribution, with the cost of a wrong ejection scaled by `fd_game_pressure` (parity-derived urgency from the `roster`). It returns SKIP / ABSTAIN / CAST. New policies should prefer `fd_vote_decision`.

**Round lifecycle:** call `fd_create` once at process start, `fd_set_self` and `fd_reset` once per round (in either order — both clear and re-establish identity), then `fd_observe_round_config(n_players, n_impostors)` once before the first `fd_run` (skip it only to accept the single-impostor default). The detector instance + compiled rules persist across rounds; only working memory is cleared.

Rule files live in `clp/` and are loaded by `fd_create(clp_dir)` at instance creation; `fd_create(NULL)` instead loads the copy embedded into the library at build time (`mk/embed-clp.sh` → `build/gen/fd_rules_embedded.c`), which needs no disk access at all. On OpenBSD the disk path implies a `pledge("stdio rpath", NULL)` window during load and a drop to `pledge("stdio", NULL)` afterwards — see `src/driver.c` for the canonical lifecycle; the embedded path can pledge `"stdio"` from the start.

## Build, run, test

```
gmake all        # Linux/macOS: build libclips.a, libfakedetector.a, build/driver
gmake test       # contract tests + run build/driver, diff stdout against tests/expected/driver.out
gmake perf       # build + run build/perf; fails if any API call blows its latency budget
gmake shared     # build/libfakedetector.so.<ABI> (fd + embedded rules + CLIPS, fd_* exported only)
gmake install    # PREFIX/DESTDIR install: header, merged .a, .so, fakedetector.pc, clp/ data
gmake clean
```

`perf` is intentionally not wired into `test` — per-call timing is environment-sensitive. Budgets are overridable per the header comment in `src/perf.c` (compile-time `-DFD_PERF_*_BUDGET_NS=...` or the matching runtime env vars).

OpenBSD / *BSD:

```
make all         # bmake reads BSDmakefile; same targets as the GNU side
make test
make clean
```

On Linux we do not have OpenBSD's `make` available, but a portable `bmake` (available from your distro, e.g. `apt install bmake`) can syntax-check and run the BSDmakefile:

```
bmake -f BSDmakefile <target>
```

Use this before claiming changes to `BSDmakefile` work.

The driver's snapshot is at `tests/expected/driver.out`. If a rule change intentionally changes output, regenerate with `./build/driver > tests/expected/driver.out` and commit the new snapshot.

## Repository layout

- `src/fakedetector.h` — the primary public header: the `fd_player` id model, phase/stance enums, lifecycle + observation + room-graph + render + query + profile functions. `src/fd_crewrift.h` is the optional CrewRift color-roster convenience header (also installed). `src/fd_internal.h` is library-private (the `fd_detector` struct, `room_graph`, cross-module helpers); consumers must not include it.
- `src/` library modules (built into `libfakedetector.a`, one responsibility each — see each file's top-of-file comment):
  - `fakedetector.c` — lifecycle (create/destroy/reset), self identity, playstyle, the player-name registry (`fd_set_player_name` / `fd_player_name`).
  - `fd_clips.c` — the only code that touches the CLIPS API directly: assert/scan/decode helpers, `fd_run`.
  - `fd_observe.c` — the `fd_observe_*` ingestion family; translates game events to fact assertions.
  - `fd_graph.c` — caller-asserted room navigation graph, all-pairs shortest paths, round-scoped closure overlay, last-sighting cache for the vent check.
  - `fd_query.c` — read-only situational queries (`fd_get_stance/evidence/status/suspicion`, `fd_explain_suspicion`, `fd_run_stats_get`, `fd_pick_vote_target`, `fd_vote_decision`, `fd_game_pressure`, `fd_win_probability`, `fd_rank_suspects`, `fd_alone_risk`, positional/round-aggregate queries). `fd_room_distance` lives in `fd_graph.c`.
  - `fd_render.c` — player-facing prose builders (vote summary, Poirot case file, per-color rationale); snprintf-style returns.
  - `fd_telemetry.c` — stable observability surface: `fd_dump_state` / `fd_dump_state_buf` / `fd_dump_state_path` (structured working-memory snapshot to a caller `FILE *`, an snprintf-style buffer, or a path — the latter two are the FFI-safe variants), `fd_trace_begin` / `fd_trace_begin_path` / `fd_trace_end` (a CLIPS router that captures rule/fact/activation watch output), and the `fd_sink` writer they share with `fd_schema.c`. `fd_explain_suspicion` / `fd_run_stats_get` sit with the queries in `fd_query.c`.
  - `fd_profile.c` — diagnostic wrappers over CLIPS `(profile)` / `(matches)`; **not** part of the stable surface.
- `src/driver.c` — dev harness with a hand-coded synthetic round. Self-tests stance/API contracts via `fd_get_stance` etc.; exits non-zero on mismatch. Not part of the library.
- `src/perf.c` — performance harness (three elaborate scenarios, per-call latency budgets). Built into `build/perf`, not the library.
- `clp/templates.clp` — fact-schema deftemplates: identity (`self-color`), raw observations (`sighting`, `body-seen`, `task-tick`, `self-state`, `player-dead`, `ejection`), dossier (`crewmember`, `near-body`), round-end marker (`impostor-found`), case-reasoning layer (`case`, `kill-window`, `alibi`, `cast-iron-alibi`, `suspect`, `accusation`, `room-distance`, `vent-suspected`), the testimony / social layer (`claim`, `contradiction`, `corroboration`, `accuses`, `defends`), the probabilistic / round-model layer (`logodds-term`, `suspicion`, `roster`, `game-pressure`), and `stance` (whose `evidence` slot names the threat source — accusation/vent/near-body/off-task/on-task/none — read by `fd_get_evidence`).
- `clp/dossier.clp` — per-color crewmember maintenance, body-proximity evidence, death/ejection promotion, body-implies-death derivation.
- `clp/cases.clp` — Sherlock/Poirot detective layer: opens a `case` per body, derives the `kill-window`, exonerates colors via `alibi` / cast-iron-alibi facts, opens and narrows the `suspect` pool, strengthens suspects from near-body and vent evidence, solves the case. Impossible-movement (vent) detection itself lives in the **C library** (`fd_graph.c`, routing-distance check) which asserts `vent-suspected` facts directly — cases.clp only consumes them.
- `clp/claims.clp` — testimony layer: turns policy-parsed vote-chat `claim` facts into deductive signal. Derives a `contradiction` when a location claim is refuted by the sighting record (geometric — reuses the `room-distance` machinery) and a `corroboration` when two colors vouch for the same room; emits `logodds-term` increments for those plus self-reports and silent witnesses (a near-body color who never speaks). Opt-in — inert until claims are fed.
- `clp/social.clp` — accusation / defense social graph: consumes `accuses` / `defends` edges and, when an ejection reveals an impostor, retro-scores every color that interacted with it (defenders penalised, accusers credited, co-accusers and packers-on-the-cleared penalised) as `logodds-term` increments. The one layer whose evidence correlates *across* players. Opt-in.
- `clp/suspicion.clp` — probabilistic joint-belief substrate and round model, loaded after `claims.clp` / `social.clp`, before `stance.clp`. Maintains the `roster` (live crew/impostor counts), derives parity-aware `game-pressure`, and translates each settled piece of *core* evidence (cast-iron-alibi, vent, accusation, near-body, off/on-task) into an immutable `logodds-term` contribution in log-odds space. The C-side `fd_normalize_suspicion` pass in `fd_run` sums every `logodds-term` (from here, `claims.clp`, and `social.clp`) into normalized `suspicion.weight` values. Asserts no verdict — `stance.clp` still does that.
- `clp/stance.clp` — three-category stance derivation with salience priority `threat (vent) > threat (case) > off-task > on-task > unknown`. Stances only fire for `(status alive)`; a status change to dead/ejected retracts the existing stance at salience 200. The round-end demotion at salience 250 is **roster-gated** — it clears lingering threats only once `impostors-ejected >= n-impostors` (so a multi-impostor round is not cleared by the first ejection), and the threat-promotion rules gate on that same all-ejected condition. With the default `n-impostors` of 1 this is identical to the old single-impostor behavior.
- `mk/sources.mk` — variables shared between both makefiles (plain `VAR = VAL`, dialect-neutral), including `FD_VERSION` / `FD_ABI` (keep in sync with the `FD_VERSION_*` / `FD_ABI_VERSION` macros in `fakedetector.h` — see `docs/abi.md`).
- `mk/embed-clp.sh` — build step converting `clp/*.clp` into `build/gen/fd_rules_embedded.c` (hex byte arrays + load-order table) for the `fd_create(NULL)` embedded path.
- `mk/fd_exports.map` / `mk/fd_exports.macos` — shared-library export lists: only `fd_*` is public; CLIPS symbols stay local.
- `mk/fakedetector.pc.in` — pkg-config template (`@PREFIX@` / `@VERSION@` substituted at build).
- `GNUmakefile` / `BSDmakefile` — entrypoint per dialect; no `Makefile` ships, so the wrong tool fails loudly.
- `vendor/clips/core/` — CLIPS 6.4.2 sources, vendored (MIT-0; see `THIRD_PARTY_NOTICES.md`). Do not modify. Only the `core/` subset is vendored.
- `bindings/python/` — the pure-Python (ctypes, stdlib-only) binding: `fakedetector/_native.py` mirrors the ABI (keep its `ABI_VERSION` and Structure layouts in sync with the header — see `docs/abi.md`), `fakedetector/api.py` is the `Detector` wrapper, `tests/test_binding.py` replays the C contract round through Python. A Nim binding is a planned follow-up (`bindings/nim/`).
- `docs/` — engineering notes; one markdown file per topic. `docs/model.md` is the fact-schema rationale; `docs/policy-guide.md` is the practical reference for writing a policy that links the library (read it before extending the public API); `docs/integration-crewrift.md` maps CrewRift's decoded observation vocabulary onto the API; `docs/abi.md` is the versioning/ABI policy; `docs/example-session.md` is a worked flee-and-vote round generated by `bindings/python/examples/flee_and_vote.py`.
