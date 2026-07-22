# Fact-schema and rule-category model

Notes on why the deftemplates and stance categories look the way they do. Read alongside `clp/templates.clp`, `clp/dossier.clp`, `clp/stance.clp`.

A scoping note for readers eyeing other games: the schema below is grounded in the Among-Us-like genre (kills, bodies, meetings, parity), and that is deliberate. The suspicion / claims / social layers are genre-general and reuse directly; the case / kill-window / vent layers and the parity round model are the genre-specific half. See the "Vocabulary and scope" section of the README for the full statement — the short version is that a new game gets ported by feeding the general channels and, where needed, adding *new* evidence templates, not by renaming these.

## Game-observation grounding

The Among Them player observes, per tick:

- `phase` ∈ {`pregame`, `playing`, `voting`, `results`, `gameover`}
- self state: `color` (0..15), `room`, `x`/`y`, `is_imposter`, `kill_ready`
- `visible_players` — list of other-color positions in line of sight
- `visible_bodies` — pixel-localized body sprites
- task counters and a `vote_slots` list during voting

**Roles are not exposed in opponent observation streams** — every verdict the detector makes is inferential. The wire model has no named "vent" or "sabotage" event; those manifest only as movement/graphical state, so threat evidence is circumstantial (proximity-to-body, impossible movement, witnessed kills near self). (Verified against the game's observation encoder when the rules were first written.)

## Three-layer fact schema

```
raw observations   ->   per-color dossier   ->   stance verdict
(asserted by C)        (derived; one fact          (one fact per color,
                        per color)                  rationale string)
```

### Identity (`self-color`)

A singleton-by-convention fact: the C side asserts/replaces exactly one `(self-color (color ?c))` at round start (via `fd_set_self`). Rules can pattern on it to filter self out of opponent-targeted reasoning, and the render API uses it for the summary header. `fd_reset` clears it; callers must re-call `fd_set_self` at the start of the next round.

### Raw observations (`sighting`, `body-seen`, `task-tick`, `self-state`, `player-dead`, `ejection`)

One fact per discrete observed thing per tick. The C side decides what's worth asserting — frame-by-frame deltas would explode the fact base, so callers should coalesce sightings and only assert when something changes meaningfully (room, task, body discovery, voting transition).

`self-state` carries `phase` and `kill-ready` (not `color` — that's the singleton `self-color` fact). `kill-ready` is reserved for impostor-side reasoning we'll add later.

`body-seen` carries the dead player's `color` (the game's snapshot encoder exposes the body's color). The `body-implies-death` rule derives a `(player-dead)` fact from any body-seen, so the caller doesn't need to call `fd_observe_death` in the common case.

`ejection` is fed by `fd_observe_ejection`, which mirrors the game's `player_ejected` event. The `was-impostor` slot carries the role reveal; when true, the `mark-ejected` rule also asserts an `(impostor-found)` marker. The round-end short-circuit no longer keys on `impostor-found` directly — it counts ejected impostors against the `roster` (see [Probabilistic belief layer and round model](#probabilistic-belief-layer-and-round-model)) — but the marker is kept for `fd_round_stats`.

### Dossier (`crewmember`, `near-body`)

One `crewmember` fact per color we have evidence of. Slots are aggregates:
- `status` ∈ {`unknown`, `alive`, `dead`, `ghost`, `ejected`} — set by `ensure-dossier-from-*` (alive), `mark-dead` (dead), `mark-ejected` (ejected). `ghost` is reserved for a later round when we ingest `visible_ghosts`.
- `last-seen-tick` / `last-seen-room` — monotonically updated; the `~dead&~ejected` guard on `update-last-seen` prevents a stale sighting from resurrecting a known-dead color.
- `visible-tasks` — running count; the `count-task` rule retracts each `task-tick` after crediting so it cannot double-count.

Evidence lives in **separate fact templates** (`near-body` for now) rather than packed into a `multislot`. This trades a few extra facts for far simpler pattern conditions in `stance.clp`.

### Stance (`stance`)

One `stance` fact per **alive** dossiered color. Slots: `category` ∈ {`unknown`, `on-task`, `off-task`, `threat`}; `evidence` ∈ {`none`, `accusation`, `vent`, `near-body`, `off-task`, `on-task`} naming the kind of observation behind the verdict (read by `fd_get_evidence` so a policy can threshold votes on evidence quality); and a freeform `rationale` STRING the C side reads verbatim when rendering the vote summary. Each promotion rule sets `category`, `evidence`, and `rationale` together. Dead and ejected colors have no stance fact (the `retract-stance-on-status-change` rule at salience 200 removes any prior stance the moment status flips away from alive).

## Player status vs stance

Two orthogonal axes. **Status** answers "is this color a valid vote target?". **Stance** answers "if alive, how suspicious does this color look?". The library APIs split accordingly:

| API                                          | Reads             |
|----------------------------------------------|-------------------|
| `fd_get_status(color)` → `fd_player_status`  | `crewmember.status` |
| `fd_get_stance(color)` → `fd_stance`         | `stance.category` (only meaningful for alive colors) |
| `fd_pick_vote_target(&color, &severity)`     | walks alive stances, picks highest severity |
| `fd_render_vote_summary(buf, n)`             | alive stances + deceased footer |

`fd_pick_vote_target` is the canonical vote-time entry point. It filters by status before considering stance, so a dead-but-previously-threat color is never returned. Tie-breaks are deterministic (ascending player id) so snapshot tests are stable.

## Risk projection

The voting question — "who is the worst alive color?" — is one shape the policy asks. During the playing phase the policy asks a different question constantly: **"if I end up alone in a room with X, how dangerous is that?"** `fd_alone_risk(fd, who)` answers that without forcing the caller to re-derive the answer from raw stance.

Risk is the **third projection** of the underlying facts, alongside per-color stance and vote-pick. All three are C-side projections over CLIPS-asserted facts; the CLIPS layer never knows or cares which projection a consumer wants.

| Inputs (in priority order)                              | `fd_alone_risk` |
|---------------------------------------------------------|-----------------|
| `who` is self                                            | `NONE`          |
| `who`'s status is `dead` / `ejected`                     | `NONE`          |
| every impostor ejected (`roster`)                        | `NONE`          |
| `who`'s status is `unknown`                              | `UNKNOWN`       |
| `who`'s stance is `threat`                               | `HIGH`          |
| `who`'s stance is `off-task`                             | `MEDIUM`        |
| `who`'s stance is `on-task`                              | `LOW`           |
| stance is `unknown` (alive, no rule fired)               | `MEDIUM`        |

`fd_alone_risk_rationale` composes a single-line explanation: a level prefix (`HIGH risk - …`) plus a body that's either a special case (self / deceased / ejected / impostor-found / never-sighted) or the underlying stance's rationale verbatim.

### Why C-side and not new CLIPS rules

The mapping above is direct enough that a CLIPS implementation would mean salience-juggling rules around "default-to-medium-unless-stance-is-better" — net negative readability. The architectural seam still holds: CLIPS owns the substantive facts (stance, status, `roster`); C owns the *shape* of the answer to each query type.

If the model later grows substantive new inputs that risk should react to — `kill_ready` proximity, per-room witness counts — the principled move will be a `(threat-level)` deftemplate fed by new rules, with `fd_alone_risk` reading the slot. The C-side projection is a deliberate placeholder until that complexity arrives.

### Multi-impostor support

Earlier the `(impostor-found)` short-circuit assumed a single impostor: ejecting one impostor dropped *every* alive color to risk `NONE`. That is now roster-driven (see [Probabilistic belief layer and round model](#probabilistic-belief-layer-and-round-model)). `fd_alone_risk` returns `NONE` for the all-clear case only once `roster.impostors-ejected >= roster.n-impostors` — in a 2-impostor round, ejecting the first impostor leaves the others at their stance-projected risk, because a killer is still loose. The C-side check is `fd_all_impostors_ejected`; with the default single-impostor roster it agrees exactly with the old `fd_impostor_found` behavior.

## Case reasoning (Sherlock/Poirot layer)

`clp/cases.clp` adds the detective-style deductive layer. The four key moves of a Holmes/Poirot detective map directly to deftemplate + rule pairs:

| Detective move                          | Templates                              | Driving rule(s) |
|-----------------------------------------|----------------------------------------|-----------------|
| Reconstruct the crime                   | `case`, `kill-window`                  | `open-case`, `init-kill-window`, `update-kill-window-lower` |
| Eliminate the impossible (alibi)        | `alibi`, `cast-iron-alibi`             | `derive-alibi`, `derive-cast-iron-alibi` |
| Detect impossible movement (vent)       | `vent-suspected`                       | asserted by the C library — see [Vent detection](#vent-detection) |
| Process of elimination → suspect pool   | `suspect`                              | `open-suspect-from-crewmember`, `retract-suspect-on-cast-iron-alibi`, `strengthen-suspect-near-body`, `strengthen-suspect-vent`, `weaken-suspect-on-weak-alibi` |
| Name the murderer                       | `accusation`                           | `solve-case` |
| Narrate the case                        | (none new)                             | `fd_render_case` in C |

### Salience layering (cases + stance combined, high → low)

```
250  clear-threats-on-impostor-found          (round-won demotion, roster-gated)
200  retract-stance-on-status-change          (dead/ejected can't be a threat)
200  retract-suspicion-on-status-change       (drop the dead from the belief pool)
160  roster-ensure, roster-recount            (live crew/impostor counts)
155  pressure-ensure, pressure-derive         (parity-derived game-pressure)
150  update-kill-window-lower                 (each victim sighting ratchets from-tick up)
140  open-case, init-kill-window              (case-derivation band; within it,
140  attach-near-body                          ordering is by fact dependency,
140  derive-alibi, derive-cast-iron-alibi      not salience -- see the cases.clp
140  open-suspect-from-crewmember              header comment)
140  retract-suspect-on-cast-iron-alibi
140  strengthen-suspect-near-body
140  strengthen-suspect-vent
140  solve-case
140  derive-contradiction-*, derive-corroboration  (claims.clp; in the case band)
130  weaken-suspect-on-weak-alibi
120  stance-accuse                            (a solved case names the murderer)
110  stance-threat-from-vent                  (vent is the strongest circumstantial signal)
100  stance-threat                            (case-based; requires no cast-iron alibi)
 95  suspicion-ensure, suspicion-prior        (bootstrap one suspicion fact per color)
 90  term-* (evidence -> logodds-term)        (suspicion.clp + claims.clp + social.clp;
                                               after the case layer has settled)
 20  stance-off-task
 10  stance-on-task
-10  stance-ensure                            (default unknown for every alive dossier)
```

The whole case-derivation layer sits in a 130–150 salience band, comfortably above every `stance-*` rule, so the facts the stance rules read (`cast-iron-alibi`, `accusation`, `near-body`) are settled before any verdict is drawn. Vent detection is **not** in this list — `(vent-suspected)` facts are asserted by the C library before `fd_run`, not by a rule. The `suspicion.clp` rules (160/155/95/90) read settled facts only and write no verdict; the `term-*` band at 90 sits below the threat rules so the case layer is final when an evidence increment is recorded.

### Why `open-suspect-from-crewmember` requires no cast-iron alibi

Without the guard, `open-suspect-from-crewmember` and `retract-suspect-on-cast-iron-alibi` would oscillate forever: open asserts a suspect → retract removes it (a cast-iron alibi exists) → open re-asserts (no suspect present) → … infinite. The `(not (cast-iron-alibi …))` clause in open-suspect breaks the cycle. `retract-suspect-on-cast-iron-alibi` is kept as a defensive cleanup for the case where a cast-iron alibi arrives after the suspect was opened (different event order). A *weak* alibi does not clear a suspect — only `cast-iron-alibi` does — so the guard names that template specifically.

### Why threat rules require the roster all-ejected guard

`clear-threats-on-impostor-found` modifies any `(stance … (category threat))` to unknown once every impostor is ejected. Without the matching `(not (roster … impostors-ejected >= n-impostors))` guard on the threat-promotion rules, the cleared stance would immediately re-promote to threat, get cleared again, and loop. The guard means once the last impostor is found, no new threats fire — and the existing ones get demoted exactly once. (Before the multi-impostor work this gate was a plain `(not (impostor-found))`; the roster form generalizes it without changing single-impostor behavior.)

### Vent detection

A `(vent-suspected)` fact is always asserted by the **C library**, never by a rule. Its `basis` slot records which of two paths produced it.

**Inferred** (`basis inferred`). Routing-aware impossible-movement detection in `src/fd_graph.c` + `vent_check` in `src/fd_observe.c`. An earlier design tested raw world-space speed against a Euclidean `vent-threshold-sqr`; that was replaced because straight-line pixel distance does not respect walls and doorways — two rooms can be close in pixels yet far apart on foot. The current check asks the caller to assert a room navigation graph via `fd_add_room_link` (one edge per adjacent room pair, cost in ticks); the library keeps an all-pairs shortest-path matrix (Floyd-Warshall, recomputed lazily, closure overlay applied). On each `fd_observe_player`, `vent_check` compares the elapsed ticks since that color's previous sighting against the shortest-path cost between the rooms; a move *faster* than the cheapest legal walk asserts the fact. With no graph asserted, no inferred check ever fires — that path is opt-in.

**Observed** (`basis observed`). `fd_observe_vent` lets the caller report a vent use it witnessed directly (e.g. a sprite at a known vent coordinate). It needs no room graph and is the stronger signal — only impostors vent. `from-room` and `to-room` are both the vent's room.

Four CLIPS rules consume `vent-suspected`, paired by `basis`: `stance-threat-from-vent` / `strengthen-suspect-vent` take the inferred facts, and `stance-threat-from-observed-vent` / `strengthen-suspect-observed-vent` take the observed ones. All four yield a `threat` stance with `evidence vent`; the split exists only so the rationale prose is honest ("impossible movement: A to B…" vs. "seen using a vent in R…"). `fd_room_distance` exposes the shortest-path matrix for callers to verify their graph. See `fd_add_room_link` and `fd_observe_vent` in `fakedetector.h`, and the policy guide's sharp-edges note on the all-pairs trap.

### `fd_render_case` — Poirot's drawing-room reveal

`fd_render_case(fd, buf, n)` walks `case` facts and, for each, composes:

```
Case file: the murder of <victim>
  Body discovered in <room> at tick <bt>.
  Kill window: ticks <from>..<to>.
  Suspects:
    <color> - <basis>
  Cleared:
    <color> - sighted in <at-room> at tick <at-tick>
```

The narrative IS the API contract — the policy pastes it into vote chat to make its case. If no body has been observed, the renderer returns a single "no bodies discovered yet" line.

## Probabilistic belief layer and round model

The categorical stance answers "*is* this color a threat" with one of four labels. It does not answer "*how much* of a threat, relative to everyone else" — and `fd_get_evidence` returns a kind, never a strength. Seven `off-task` colors are genuinely interchangeable under that model, so a vote among them collapses to player-id order. `clp/suspicion.clp` and four deftemplates add the missing magnitude and a model of how close the round is to a loss. The verdict still belongs to `stance.clp`; this layer feeds decisions, it does not draw them. (Design lineage: the literature in `docs/bibliography.md`.)

### `logodds-term` → `suspicion` — a normalized belief distribution

Every settled piece of evidence contributes a fixed **log-likelihood-ratio increment** for the implicated color, recorded as an immutable `(logodds-term (color)(source)(key)(amount))` fact. One `term-*` rule per evidence channel asserts it, each guarded so it fires exactly once per evidence instance, most wrapped in a `logical` CE so the term retracts if its evidence does. Addition in log space is multiplication of likelihoods, which keeps the rule base monotone and order-independent. The increments, in milli-nats (1000 = 1.0 nat) and tunable:

| Source | Increment | Source | Increment |
|---|---|---|---|
| cast-iron-alibi | −8000 | silent-witness | +700 |
| observed vent | +3000 | near-body (no cast-iron) | +600 |
| contradiction | +2500 | social-pack-cleared | +500 |
| inferred vent | +2000 | self-report | +400 |
| accusation | +1500 | social-co-accuse | +300 |
| social-defend-impostor | +1000 | off-task | +150 |
| | | co-location / on-task | −400 / −500 |
| | | social-accuse-impostor | −600 |

The first six rows are the core evidence (`suspicion.clp`); the remainder are
the testimony and social feeders (`claims.clp`, `social.clp` — see below).

A C-side arithmetic pass — `fd_normalize_suspicion`, run at the end of every `fd_run` — sums the terms per alive non-self color, maps each sum through an integer logistic, and normalizes the results into `suspicion.weight` values: per-mille, summing to 1000 across the pool. Because it recomputes from the immutable term facts every call, it is idempotent (every getter triggers a `fd_run`). This is the **one** place C touches "who is suspicious", and it is sanctioned because it is arithmetic, not judgement — CLIPS still decides who gets which `logodds`; C only performs the division and renormalization. Clearing a color (its `suspicion` fact retracted when it dies, or its `logodds` floored by a cast-iron alibi) mechanically lifts every survivor's weight — evidence about one propagates to all.

Read a color's weight with `fd_get_suspicion` (0..1000, −1 if no fact); `fd_rank_suspects` ranks by it. The per-term breakdown the normalization pass sums is itself observable: `fd_explain_suspicion` walks the `(logodds-term)` facts for one color and returns each `source`/`key`/`amount` alongside the summed `logodds`, the pre-normalization `likelihood`, and the final `weight` — so `logodds-term` is now a public *attribution* surface, not just an internal accumulator. It is the intended way for a policy author to see which evidence channel is driving (or mis-driving) a verdict while tuning the increments above.

### `roster` and `game-pressure` — the multi-impostor / parity model

`fd_observe_round_config(n_players, n_impostors)` is called once per round; it asserts the singleton `roster`. `roster-recount` keeps the derived counts current as deaths and ejections arrive (`alive-crew`, `alive-impostors`, `impostors-ejected`). `n-impostors` defaults to 1 when round-config is never called, so every pre-existing single-impostor path is unchanged.

`game-pressure` is derived from `surplus = alive-crew − alive-impostors` (crew lose at surplus ≤ 0), with a parity bump — an odd surplus reads one band hotter than the even one above it (Migdał: adding one townsperson can help the mafia). `fd_game_pressure` exposes the level; `fd_win_probability` exposes `w(n,m)` in per-mille, an integer DP over the Mafia death process, for instrumentation.

### `fd_vote_decision` — expected-value vote gating

`fd_pick_vote_target` picks by stance tier and is kept for compatibility. `fd_vote_decision` is the belief-aware picker: it takes the highest-`weight` alive color and runs an expected-value test — `EV(cast) = P(impostor)·gain − P(innocent)·loss`, with `P(impostor) = weight/1000` and `loss` scaled by `game-pressure` (a wrong ejection near parity is near-fatal, so the bar to cast drops as pressure rises). It returns `FD_VOTE_CAST` when EV is positive and the confidence margin over the runner-up clears a bar, `FD_VOTE_SKIP` when the top color does not beat the `m/n` prior (nobody looks worse than chance), and `FD_VOTE_ABSTAIN` when the distribution is too flat to act on. The `rationale` field states the skip/abstain reason in prose.

### Testimony and social feeders (`claims.clp`, `social.clp`)

The core evidence channels above (vents, body-proximity, alibis) are *rare* — most rounds the policy witnesses none of them, and the eval found the detector then collapses everyone to `off-task`. The feeder layers open two *abundant* channels. Both are **opt-in**: with no `claim` / `accuses` / `defends` facts asserted, no rule here fires, and behavior is byte-identical to before.

**Testimony — `clp/claims.clp`.** The policy does the NLP on `vote_chat_text` and passes structured `claim` facts via `fd_observe_claim` (consistent with the "caller perceives, library deduces" seam). A `claim`'s `tick` is the playing-phase tick it *refers to*, not the utterance tick. Three derived signals:

- **Contradiction.** A `location`/`task` claim refuted by the sighting record — placed in a different room at the same tick, or in a room unreachable from a confirmed sighting within the elapsed ticks (the `room-distance` test that powers `derive-cast-iron-alibi`). A `contradiction` is *geometric*, not behavioral: unlike fake-tasking it cannot be explained away, so it carries a large increment.
- **Corroboration.** Two colors who each claimed the same room within `?*claim-overlap-window*` ticks, neither contradicted, weakly clear each other — a mutual alibi (small negative for both). The `(not (contradiction …))` guards sit inside the rule's `logical` block, so a contradiction derived later retracts the corroboration.
- **Silent witness.** A `near-body` color who files no self-report and no location claim about the kill room — an innocent witness would normally speak. Gated on `(exists (claim))`: silence is informative only when the channel is in use.

**Social graph — `clp/social.clp`.** `fd_observe_vote` / `fd_observe_defense` record `accuses` / `defends` edges. These are inert until an `ejection … was-impostor t` reveals an impostor — then the whole table is **retro-scored**: a color that defended the revealed impostor draws a large increment, one that accused it is credited, one that co-accused the impostor's target or piled onto a cast-iron-cleared color draws a small one. This is the only layer whose evidence *correlates across players* — a single reveal re-reads everyone who interacted with the revealed impostor.

Both layers assert no verdict; every signal is a `logodds-term` increment feeding the normalization above.

## Why these three categories

The user-facing taxonomy maps to evidence strength:

| Category    | Evidence required                                    | Voting implication |
|-------------|------------------------------------------------------|--------------------|
| `on-task`   | ≥ 2 visible task completions, no body-proximity hits | trust this color   |
| `off-task`  | alive but no observable task progress                | mild suspicion     |
| `threat`    | in body's room within Δ=120 ticks before discovery   | call them out      |
| `unknown`   | no rule's preconditions yet met                      | abstain on color   |

`unknown` is not a rule — it's the deftemplate default. Every dossiered crewmember gets a `stance` fact with `category unknown` via the lowest-salience `stance-ensure` rule; higher-salience rules then promote.

## Salience priority

Salience runs higher-first, so the strongest verdict wins:

```
threat   (100)  > off-task  (20)  > on-task  (10)  > stance-ensure (-10)
```

Each rule's pattern excludes its own category **and** any higher-priority category. That guarantees:
- A rule cannot self-fire after modifying the fact (no infinite loop).
- A rule cannot demote a worse verdict to a better one (so an `on-task` rule never overwrites a `threat`).

`on-task` does override a prior `off-task` if task progress eventually accumulates, because `off-task` is not in `on-task`'s exclusion list — only `~on-task&~threat` is.

## What is deliberately not modeled (yet)

- **Impostor-side strategy.** Detector assumes self is crewmate. The impostor's vote logic (bandwagon on accused players, deflect), plus ingestion of the impostor-POV-only `kill_executed` event, would be a separate ruleset.
- **Sabotage events beyond route closures.** Sabotage manifests in the observation stream only as movement/graphical state. Route closures are modeled (`fd_observe_route_*`); oxygen/reactor-style sabotage timers are not.
- **Raw chat text (`vote_chat_text`).** The library does no NLP. *Structured* claims, votes, and defenses parsed out of chat by the policy are now modeled (`fd_observe_claim` / `fd_observe_vote` / `fd_observe_defense` → `clp/claims.clp`, `clp/social.clp` — see [Testimony and social feeders](#testimony-and-social-feeders-claimsclp-socialclp)); the unstructured text itself stays the policy's job.
- **The 40 named task stations.** A `task-tick` is just "a visible task completion happened in room X"; we don't distinguish "Empty Garbage" from "Fix Wires".
- **Ghosts (`visible_ghosts`).** Status enum reserves `ghost`, but no observation feeds it yet.
- **Body-report attribution.** The game's simulator tracks who reported a body internally, but the observation surface available to a policy does not currently expose the reporter's identity. Until it does (or until a consumer derives it from its own perception during the meeting-call intro), there is nothing to assert. `stance-threat` therefore flags any color seen near a body within Δ ticks, with no body-report-based exoneration. Revisit when that gap closes.

These are extension points, not bugs. The current schema accommodates them without surgery: most additions are new evidence templates and additional `stance-*` rules at appropriate salience.
