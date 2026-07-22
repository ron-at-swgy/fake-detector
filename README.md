# fake-detector

A detective brain for social-deception games, as a linkable C library. The
caller feeds it per-tick observations — who was sighted where, bodies, task
progress, votes, chat claims — and the library maintains a calibrated
per-player belief distribution over "who is the impostor", a categorical
stance per player, Poirot-style case files, and an expected-value vote
recommendation, each with a natural-language rationale suitable for the
game's vote-chat slot.

Deduction runs on a vendored [CLIPS 6.4.2](https://clipsrules.net) rules
engine; the C layer owns ingestion, arithmetic, queries, and prose. The
library was built for **Among Them** (an Among Us clone) and generalizes to
games in that genre: hidden hostile roles, circumstantial evidence, public
meetings and ejection votes.

## Quickstart

```
gmake all        # Linux/macOS: build build/libclips.a, build/libfakedetector.a, build/driver
gmake test       # contract tests + golden-snapshot diff of the dev harness
gmake perf       # per-call latency budgets (informational; environment-sensitive)
```

On OpenBSD / *BSD, `make` reads `BSDmakefile` and provides the same targets.

Minimal consumer:

```c
#include "fakedetector.h"
#include "fd_crewrift.h"   /* optional: ready-made CrewRift color roster */

fd_detector *fd = fd_create(NULL);         /* NULL = rules embedded in the lib */
fd_crewrift_register(fd);                  /* ids 0..15 named red, blue, ...  */
fd_reset(fd);
fd_set_self(fd, 0);                        /* which player am I? (red)        */
fd_observe_round_config(fd, 8, 2);         /* 8 players, 2 impostors          */

/* ... every time your policy sees something worth reporting ... */
fd_observe_player(fd, tick, /*green*/ 2, "cafeteria", x, y);
fd_observe_body(fd, tick, /*blue*/ 1, "reactor", x, y);

/* ... at voting time ... */
fd_run(fd);
fd_vote_decision_t d;
fd_vote_decision(fd, &d);                  /* SKIP / ABSTAIN / CAST + target + rationale */

char chat[512];
fd_render_vote_summary(fd, chat, sizeof chat);  /* prose for the vote-chat slot */

fd_destroy(fd);
```

Players are plain integer ids (`fd_player`, 0..31); register display names
with `fd_set_player_name` (or the `fd_crewrift.h` one-liner) and every
rationale speaks the game's language. Rule files ship embedded in the
library — `fd_create(NULL)` needs nothing on disk; pass a directory instead
to iterate on `clp/` without rebuilding.

`src/driver.c` is a complete worked example (a hand-coded synthetic round),
and `docs/policy-guide.md` is the practical reference for writing a policy
against the library.

## Architecture

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
fd_get_suspicion(color)   --    reads suspicion.weight  <-- (calibrated 0..1000)
fd_explain_suspicion(c)   --    walks logodds-term      <-- (per-term attribution)
fd_alone_risk(color)      --    stance + roster         <-- (safety query)
fd_reset()                -->   Reset(env)                  (between rounds)
```

Three architectural seams keep the design honest:

1. **C never decides who is fake.** Every verdict is a CLIPS-derived fact; C
   reads slots. The one sanctioned exception is arithmetic, not judgement:
   `fd_run` normalizes the rule-produced log-odds accumulators into per-mille
   suspicion weights.
2. **CLIPS never formats player-facing text.** Rules write a rationale
   string; C wraps it in the final prose.
3. **The library is observation-cadence-agnostic.** It owns no clock and no
   socket. The caller asserts observations when convenient and calls
   `fd_run` when it wants rules to fire.

## What it computes

Three orthogonal per-player axes:

- **Status** (`fd_get_status`): alive / dead / ejected — is this player
  still a vote target?
- **Stance** (`fd_get_stance`): on-task / off-task / threat / unknown — the
  categorical read of an alive player, with `fd_get_evidence` naming the
  evidence kind behind it.
- **Suspicion** (`fd_get_suspicion`): the magnitude — a normalized per-mille
  belief weight (0..1000, summing to 1000 across the alive pool), built from
  immutable log-odds evidence terms that `fd_explain_suspicion` can attribute
  term-by-term.

Layered evidence channels, each opt-in and inert until fed:

- **Dossier** — per-player sighting/task bookkeeping, body proximity.
- **Cases** — per-body case files: kill windows, alibis, cast-iron alibis,
  suspect pools, accusations (`fd_render_case` narrates them).
- **Vent detection** — routing-aware impossible-movement inference over a
  caller-supplied room graph, plus directly-observed vent use.
- **Claims** — structured testimony: geometric contradictions,
  corroborations, silent witnesses.
- **Social graph** — accuse/defend edges, retro-scored when an ejection
  reveals an impostor.
- **Round model** — live roster, parity-derived game pressure
  (`fd_game_pressure`), win probability (`fd_win_probability`), and the
  expected-value vote gate (`fd_vote_decision`).

## Vocabulary and scope

The fact schema speaks the hidden-role / social-deduction genre's language.
Read the terms generically:

- **impostor** — a hidden hostile role among the players.
- **task** — a verifiable pro-social action; doing them is weak evidence of
  innocence.
- **body** — discovered evidence that a player was eliminated.
- **vent** — physically-impossible traversal; movement only the hostile
  role can perform.
- **ejection** — a public elimination by vote, optionally revealing the
  eliminated player's role.
- **claim / accusation / defense** — structured testimony parsed out of
  chat by the caller (the library does no NLP).

An honest scoping note: the library targets Among-Us-like games first. The
**suspicion substrate** (log-odds evidence accumulation, normalized belief
weights), the **claims layer** (contradiction / corroboration geometry),
and the **social graph** (accuse/defend edges with post-reveal rescoring)
are genre-general — a Diplomacy adapter would reuse them directly, since
every channel is opt-in and unfed channels cost nothing. The **detective
case layer** (bodies, kill windows, alibis, vents, room graph) and the
**parity round model** (`fd_game_pressure`, `fd_win_probability`) are
specific to games with kills, meetings, and a Mafia-style loss condition —
a Diplomacy port would leave them inert and need *new* channels (dyadic
trust, promises and betrayals, a non-parity win model) rather than renamed
ones. Roughly: the belief half generalizes, the whodunnit half is
genre-specific by design.

## Round lifecycle

Call `fd_create` once per process; per round call `fd_set_self` and
`fd_reset` (either order), then `fd_observe_round_config(n_players,
n_impostors)` before the first `fd_run`. The instance and compiled rules
persist across rounds; only working memory clears.

## Bindings

`bindings/python/` ships a pure-Python (ctypes, stdlib-only) binding — see
its README for Docker-friendly install steps. Nim consumers link the
static archives directly (`libfakedetector.a libclips.a -lm`); a wrapper
module is a planned follow-up.

## Documentation

- `docs/model.md` — the fact-schema and rule-category rationale.
- `docs/policy-guide.md` — how to write a game policy that links the library.
- `docs/integration-crewrift.md` — mapping CrewRift's decoded observations
  onto the API.
- `docs/abi.md` — versioning and ABI policy for binding authors.
- `docs/bibliography.md` — the literature behind the belief layer.

## License

MIT — see `LICENSE`. The vendored CLIPS engine is MIT-0 — see
`THIRD_PARTY_NOTICES.md`.
