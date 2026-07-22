# Policy-developer guide

A practical reference for writing an Among Them player policy that links against `libfakedetector.a`. Aimed at a competent C developer who has never touched this library; expected reading time ~15 minutes. Read top-to-bottom; the [sharp edges](#sharp-edges) section near the end is required reading before you ship.

---

## 1. What this library is

`libfakedetector` is a CLIPS-backed deductive engine for a single Among Them player. You feed it observations as you make them (sightings, bodies, tasks, ejections, sabotage, and — parsed from vote-chat — claims, votes, and defenses). It maintains a dossier on every color it has seen, derives a per-color *stance* (objective suspicion judgement: on-task / off-task / threat / unknown), keeps a calibrated probability distribution over who is the impostor, and answers three policy-facing questions:

- **"Am I safe here?"** — for each color: how risky is being alone with them? (`fd_alone_risk`)
- **"How suspicious is each color, on a scale?"** — a normalized per-mille belief weight (`fd_get_suspicion`).
- **"Who should I vote for, and is it worth it?"** — at vote time, an expected-value recommendation (`fd_vote_decision`), or the legacy stance-tier pick (`fd_pick_vote_target`).

It does **not** decide what you should *do*. It tells you what's true given the evidence. The policy layer owns movement, voting decisions, body-reporting calls, and chat. Think of it as Poirot at your shoulder, not the captain at your back.

**It assumes you can identify players.** Every observation and query is keyed on a stable `fd_player` — a plain integer id 0..`FD_MAX_PLAYERS`-1 that you choose and keep stable (a color index, a seat number, a join order). The detector reasons about "player 4", never "a crewmate"; register display names with `fd_set_player_name` (or `fd_crewrift_register` from `fd_crewrift.h` for the CrewRift color roster) and every rationale uses them. A policy whose perception does *not* track identity — e.g. computer vision that wildcards the suit — must build that identity mapping itself before it can call in.

---

## 2. The shape of a session

One `fd_detector *` lives for the whole game (often many rounds). Inside that lifetime, four kinds of work happen, looping:

```
+-----------+      configure once per game
| fd_create | ---> fd_set_self, fd_set_playstyle, fd_add_room_link*
+-----------+
      |
      v
+----------------------+   once per round, before observing
| fd_observe_round_config | (n_players, n_impostors)
+----------------------+
      |
      v
+--------------------+     observe each tick
| fd_observe_*       | <-- fd_observe_self/player/body/task/death
| fd_observe_route_* |     fd_observe_route_closed/_opened (sabotage)
+--------------------+
      |
      v
+--------------------+     query at decision points
| queries            | <-- fd_alone_risk, fd_vote_decision,
+--------------------+     fd_get_suspicion, fd_pick_vote_target,
      |                    fd_rank_suspects, fd_room_occupants,
      |                    fd_last_seen, fd_round_stats_get, ...
      v
+----------+      between rounds
| fd_reset | ---> then fd_set_self + fd_observe_round_config again
+----------+
      |
      |  ...repeat until game ends
      v
+------------+
| fd_destroy |
+------------+
```

Observations are cheap (one fact assertion); queries do the work (each implicitly runs the rule engine to fixpoint). Configure once, observe a lot, query when needed, reset between rounds.

---

## 3. Setup

Call these once, in order, at the start of the game:

```c
#include "fakedetector.h"
#include "fd_crewrift.h"   /* optional CrewRift color roster */

fd_detector *fd = fd_create(NULL);       /* NULL = embedded rules; or a clp/ path */
if (fd == NULL) { /* rules invalid (or, with a path, files missing) */ }

fd_crewrift_register(fd);                 /* name ids 0..15 red, blue, ... —
                                             or fd_set_player_name per id */
fd_set_self(fd, 0);                       /* who you are this round (red) */

fd_set_playstyle(fd, FD_PLAYSTYLE_NEUTRAL); /* optional; default is NEUTRAL */

/* Optional: assert the navigation graph for vent detection. Skip this
 * entirely if you don't want vent detection. Assert ONE link per
 * adjacent room pair — a sparse graph — with the cost being the
 * minimum honest walk time, in TICKS (~25/sec), through that doorway.
 * Multi-room costs are composed for you; do NOT assert all-pairs. */
fd_add_room_link(fd, "cafeteria", "medbay",  50);
fd_add_room_link(fd, "cafeteria", "admin",   80);
fd_add_room_link(fd, "medbay",    "storage", 50);
fd_add_room_link(fd, "medbay",    "admin",  100);
/* ... etc. for every adjacent room pair ... */
```

Notes:
- `fd_create(NULL)` defaults to `"./clp"`.
- Verify the graph once with `fd_room_distance(fd, a, b)` — see the [sharp edges](#sharp-edges) note on the all-pairs trap.
- The graph **persists across `fd_reset`** — game-world topology doesn't change between rounds. Assert it once.
- Playstyle also persists across `fd_reset`.
- On OpenBSD: `fd_create` does file I/O. Call it inside a `pledge("stdio rpath", NULL)` window; drop to `pledge("stdio", NULL)` afterwards. `unveil("clp", "r")` works too.

### Playstyles in one line each

| Constant | Behavior |
|---|---|
| `FD_PLAYSTYLE_TRUSTING` | Vote only on confirmed threats; risk projections damped. |
| `FD_PLAYSTYLE_NEUTRAL` | Default — vote threat/off-task/on-task in priority order. |
| `FD_PLAYSTYLE_PARANOID` | Vote anyone — even an unknown — if no one else is worse. |

The setting biases `fd_alone_risk`, `fd_pick_vote_target`, `fd_rank_suspects`, and the rationale prefix. Stance derivation stays objective regardless.

---

## 4. Observation feed

Call these every tick (or whenever the corresponding game event fires). All asserts are sub-microsecond; cost shows up at query time, not here.

| Function | When to call | What it asserts |
|---|---|---|
| `fd_observe_round_config(fd, n_players, n_impostors)` | Once per round, after `fd_set_self`, before the first query. | A `(roster ...)` fact. Skipping it accepts a single-impostor default. `n_impostors` drives the round-end demotion and `fd_game_pressure`. |
| `fd_observe_self(fd, tick, phase, room, x, y, kill_ready)` | Every tick. `phase` is one of `FD_PHASE_PREGAME/PLAYING/VOTING/RESULTS/GAMEOVER`. | A `(self-state ...)` fact carrying your position. Used by the case-render to anchor the timestamp. |
| `fd_observe_player(fd, tick, who, room, x, y)` | Whenever you see another player. `who` is *their* color. | A `(sighting ...)` fact. Updates that color's dossier. **Silently dropped if `who == self`.** Triggers vent check against your room graph. |
| `fd_observe_body(fd, tick, color, room, x, y)` | When you see a body. `color` is the victim's color. | A `(body-seen ...)` fact, which cascades to open a case, derive the kill-window, generate alibis and suspects. |
| `fd_observe_task_completion(fd, tick, who, room)` | When you see someone complete a task. | A `(task-tick ...)` fact that increments their visible-task count. After 2+ tasks the dossier marks them on-task. |
| `fd_observe_death(fd, tick, who)` | When you infer a death without seeing the body (vote-slot delta, etc.). | A `(player-dead ...)` fact. Idempotent. |
| `fd_observe_ejection(fd, tick, who, was_impostor)` | After a vote ejects someone. `was_impostor` mirrors the game's role-reveal flag. | An `(ejection ...)` fact. The detector counts impostor ejections against the round's `roster`; threat stances are cleared only once *every* impostor is accounted for (so the first of two ejections does not blind the detector). |
| `fd_observe_vent(fd, tick, who, room)` | When you *directly witness* a player enter/exit a vent. | A `(vent-suspected ... basis observed)` fact → promotes `who` to `THREAT` (`FD_EVIDENCE_VENT`). The confirmed counterpart to the room-graph's inferred vent check; needs no graph. Pair it with the `fd_observe_player` sighting from the same look. |
| `fd_observe_route_closed(fd, room_a, room_b)` | When sabotage / lock blocks a route. | A round-scoped closure overlay on the graph. Reopened by `fd_reset`. |
| `fd_observe_route_opened(fd, room_a, room_b)` | When sabotage clears / lock unlocks. | Reverses the closure. Idempotent. |
| `fd_observe_claim(fd, tick, who, kind, room)` | When you parse a location / task / self-report claim out of vote-chat. `tick` is the tick the claim is *about*. | A `(claim ...)` fact. A location claim refuted by the sighting record raises suspicion sharply; two players vouching for one room weakly clear each other. |
| `fd_observe_vote(fd, tick, voter, target)` | When a vote is cast / a chat accusation is made. | An `(accuses ...)` edge in the social graph; retro-scored when an ejection reveals an impostor. |
| `fd_observe_defense(fd, tick, defender, defended)` | When you parse a defense out of vote-chat. | A `(defends ...)` edge; defending a revealed impostor is a strong suspicion increment. |

You do **not** need to call `fd_observe_player` on yourself — call `fd_observe_self` instead, and observe *other* players via `fd_observe_player`.

### The testimony and social channels are opt-in

`fd_observe_claim` / `fd_observe_vote` / `fd_observe_defense` open two abundant
evidence channels — chat claims and the accusation graph — to compensate for how
rarely a round yields a hard signal (a real vent, a body-proximity hit). The
*policy* does the NLP on `vote_chat_text` and feeds `fd` the structured result;
the library checks claims against its own observation record (a location claim
contradicted by geometry cannot be explained away) and re-scores the social
graph whenever an ejection reveals an impostor. Both channels are **opt-in**:
feed nothing and behavior is unchanged. Their effect surfaces through
`fd_get_suspicion` and `fd_vote_decision` — they add no new query.

---

## 5. Querying mid-round

The detector can answer per-color, per-room, and aggregate questions while the round is in progress. Use these to drive movement and safety decisions.

### Per-color: status, stance, risk

```c
fd_player_status fd_get_status(fd_detector *, fd_player);
fd_stance        fd_get_stance(fd_detector *, fd_player);
fd_evidence      fd_get_evidence(fd_detector *, fd_player);
fd_risk          fd_alone_risk(fd_detector *, fd_player);
long             fd_get_rationale(fd_detector *, fd_player, char *buf, size_t bufsize);
long             fd_alone_risk_rationale(fd_detector *, fd_player, char *buf, size_t bufsize);
```

See [the glossary](#7-stance-vs-status-vs-risk) for the three axes. `fd_alone_risk` is the projection that respects your playstyle; the others are raw deduction. `fd_get_evidence` reports *why* a stance was reached — `ACCUSATION`, `VENT`, `NEAR_BODY`, `OFF_TASK`, `ON_TASK`, or `NONE` — so a policy can threshold on evidence quality (see [voting time](#6-voting-time)).

### Per-color: calibrated suspicion (a magnitude, not a label)

```c
int fd_get_suspicion(fd_detector *, fd_player);   /* 0..1000 per mille, -1 if none */
```

Stance is categorical: seven `off-task` colors are indistinguishable under it. `fd_get_suspicion` returns a calibrated belief weight instead — a per-mille share of a normalized probability distribution over who is the impostor. The weights of all alive non-self colors sum to 1000, so the number is comparable across colors and a real scalar to threshold or rank on. Every piece of evidence (vents, accusations, body-proximity, alibis, task progress) shifts it; clearing one color lifts the rest. A `-1` return means no suspicion fact — self, dead, ejected, or never sighted. `fd_rank_suspects` sorts by this weight.

### Round model: pressure and win probability

```c
fd_game_pressure_t fd_game_pressure(fd_detector *);   /* LOW/MEDIUM/HIGH/CRITICAL */
int                fd_win_probability(fd_detector *); /* w(n,m), 0..1000 per mille */
```

Driven by the `roster` you set with `fd_observe_round_config`. Pressure rises as the live crew count approaches the loss condition; `fd_vote_decision` consumes it directly (see [voting time](#6-voting-time)). `fd_win_probability` is instrumentation — a calibrated read on how close the round is to a loss.

### Positional: who's in this room? Where was X?

```c
int fd_room_occupants(fd_detector *, const char *room,
                      fd_player *out_colors, int max);

int fd_last_seen(fd_detector *, fd_player,
                 char *out_room, size_t bufsize, int *out_tick);
```

`fd_room_occupants` returns colors whose *last-known* room is `room` and who are alive. It's the detector's best guess at "right now"; if you haven't seen YELLOW since they ducked into electrical 200 ticks ago, that's where YELLOW will appear. Use a recency filter via `fd_last_seen` if you care.

### Ranking: top-N suspects

```c
int fd_rank_suspects(fd_detector *,
                     fd_player *out_colors, fd_stance *out_stances, int max);
```

Like `fd_pick_vote_target` but returns the whole sorted list. Useful if your policy wants a backup vote, or wants to know that "everyone is unknown" before deciding to abstain.

### Aggregate: round stats

```c
typedef struct {
    int alive, dead, ejected, never_sighted;
    int open_cases, vent_suspicions, impostor_found;
} fd_round_stats;

void fd_round_stats_get(fd_detector *, fd_round_stats *out);
```

Endgame heuristics: when `alive == 2` and `impostor_found == 0`, every meeting is decisive. When `vent_suspicions > 0`, there's a paper trail you can cite in chat.

### Worked example — "should I follow them to medbay?"

```c
fd_player occupants[FD_MAX_PLAYERS];
int n = fd_room_occupants(fd, "medbay", occupants, FD_MAX_PLAYERS);
int safe = 1;
for (int i = 0; i < n; i++) {
    if (fd_alone_risk(fd, occupants[i]) >= FD_RISK_HIGH) {
        safe = 0;
        break;
    }
}
if (safe) {
    move_toward("medbay");
} else {
    stay_with_group();
}
```

### Worked example — "who's been missing for a suspicious length of time?"

```c
int now_tick = current_game_tick();
for (int c = 0; c < FD_MAX_PLAYERS; c++) {
    if (fd_get_status(fd, (fd_player)c) != FD_PLAYER_STATUS_ALIVE)
        continue;
    char where[32];
    int  last_t;
    if (fd_last_seen(fd, (fd_player)c, where, sizeof where, &last_t) != 1)
        continue;
    if (now_tick - last_t > 200) {  /* 8 sec at 25 ticks/sec */
        log_suspicion("%s last seen in %s %d ticks ago",
            fd_player_name(fd, (fd_player)c), where, now_tick - last_t);
    }
}
```

---

## 6. Voting time

When the meeting phase opens, render the case-file and decide:

```c
char buf[2048];
fd_render_vote_summary(fd, buf, sizeof buf);   /* one-line stance summary  */
print_to_chat(buf);

fd_render_case(fd, buf, sizeof buf);           /* Poirot-style case prose  */
log(buf);

fd_player   tgt;
fd_stance  sev;
int rc = fd_pick_vote_target(fd, &tgt, &sev);
if (rc == 1) {
    cast_vote(tgt);
} else {
    abstain();   /* nobody meets the playstyle's bar */
}
```

For backup choices (e.g., if your top pick splits the vote), pull the ranked list:

```c
fd_player  rc_colors[5];
fd_stance rc_stances[5];
int n = fd_rank_suspects(fd, rc_colors, rc_stances, 5);
/* rc_colors[0..n-1] is the priority order, top to bottom */
```

`fd_pick_vote_target` returning `0` (no target) is **not** an error. The current playstyle just doesn't want to vote anyone. TRUSTING abstains when there's no threat; PARANOID returns 0 only if there are no alive non-self colors at all.

### The belief-aware decision: `fd_vote_decision`

`fd_pick_vote_target` picks by stance tier — it cannot tell a barely-suspicious target from a near-certain one, and an at-chance vote is worse than abstaining. `fd_vote_decision` closes that gap. It runs an expected-value test over the calibrated `fd_get_suspicion` distribution, scales the cost of a wrong ejection by `fd_game_pressure`, and hands back a structured recommendation:

```c
fd_vote_decision_t d;
fd_vote_decision(fd, &d);
switch (d.recommendation) {
case FD_VOTE_CAST:    cast_vote(d.target); break;  /* d.suspicion/d.confidence quantify it */
case FD_VOTE_SKIP:    skip_vote();         break;  /* nobody beats the 1-in-N prior      */
case FD_VOTE_ABSTAIN: abstain();           break;  /* distribution too flat to act on    */
}
log(d.rationale);   /* prose — states the skip/abstain reason */
```

`d.suspicion` is the target's per-mille weight; `d.confidence` is its margin over the runner-up. CAST fires only when expected value is positive *and* confidence clears a bar, so a paranoid wrong-ejection spree is structurally discouraged. Near parity (`CRITICAL` pressure) the bar drops — a wrong *skip* now loses the game. This is the recommended vote entry point for new policies; `fd_pick_vote_target` stays for compatibility and for callers that want the raw stance-tier pick.

### Thresholding on evidence quality

`fd_pick_vote_target` gives you a color and a severity (`fd_stance`), but two `THREAT`s are not equal: one may rest on a solved case or a real vent, another on the rule engine simply running out of better options. `fd_get_evidence` tells them apart. A policy that would rather abstain than eject an innocent on thin evidence can gate on it:

```c
fd_player  tgt;
fd_stance sev;
if (fd_pick_vote_target(fd, &tgt, &sev) == 1) {
    fd_evidence why = fd_get_evidence(fd, tgt);
    if (why == FD_EVIDENCE_NEAR_BODY || why == FD_EVIDENCE_VENT ||
        why == FD_EVIDENCE_ACCUSATION) {
        cast_vote(tgt);          /* hard evidence — act on it */
    } else {
        abstain();               /* OFF_TASK-only — too weak to eject */
    }
} else {
    abstain();
}
```

`FD_EVIDENCE_NONE` for an alive color means "no read yet"; treat it as you would `FD_STANCE_UNKNOWN`.

---

## 7. Stance vs status vs risk

Three orthogonal axes; do not confuse them.

| Axis | Type | Values | Meaning |
|---|---|---|---|
| **status** | `fd_player_status` | `UNKNOWN`, `ALIVE`, `DEAD`, `EJECTED` | Whether this color is still in play. Dead/ejected colors are filtered out of vote-pick and alone-risk. |
| **stance** | `fd_stance` | `UNKNOWN`, `ON_TASK`, `OFF_TASK`, `THREAT` | How suspicious an *alive* color looks based on objective evidence. Dead/ejected colors have no stance. |
| **risk** | `fd_risk` | `UNKNOWN`, `NONE`, `LOW`, `MEDIUM`, `HIGH` | Policy projection of stance through the current playstyle, plus safety short-circuits (self, dead, impostor-already-found). The number that drives your action. |

Stance is the deductive output. Risk is the policy projection. Status is orthogonal.

**Disambiguating "unknown":** `fd_get_stance` returns `UNKNOWN` both for "never sighted" and for "alive, but no stance has crystallized yet". If you need to tell them apart, check `fd_get_status` first:

```c
fd_player_status s = fd_get_status(fd, who);
if (s == FD_PLAYER_STATUS_UNKNOWN) {
    /* No observations on this color this round. */
} else if (s == FD_PLAYER_STATUS_ALIVE &&
           fd_get_stance(fd, who) == FD_STANCE_UNKNOWN) {
    /* Alive but evidence is thin. */
}
```

---

## 8. Round lifecycle

Between rounds, call:

```c
fd_reset(fd);                       /* clears WM, closures, last-sighting cache */
fd_set_self(fd, my_color);          /* MUST re-call: reset clears self-color    */
fd_observe_round_config(fd, 10, 2); /* re-call if the next round isn't 1-impostor */
```

What `fd_reset` clears:
- Working memory (sightings, bodies, tasks, deaths, ejections, derived facts).
- The route closure overlay (sabotages reopen).
- The per-color last-sighting cache used for vent detection.
- The self-color.
- The round config — `n_impostors` falls back to the single-impostor default, so re-call `fd_observe_round_config` for any multi-impostor round.

What `fd_reset` **preserves**:
- The room graph (game-world topology).
- The playstyle.
- Loaded CLIPS rules (no recompile).

So a typical round-loop boundary is just `fd_reset` + `fd_set_self`. Between *games* you can either `fd_destroy` + `fd_create` (clean slate) or keep the same detector with the graph already loaded.

---

## 9. Sharp edges

Read this before shipping policy code.

- **Tick units are game ticks, not seconds.** The game runs at ~25 ticks per second. A "15-tick" window is ~0.6 sec. Costs in `fd_add_room_link` are in ticks too; adjacent-room walks are typically 30–80 ticks (1.2–3.2 sec).

- **The detector is not thread-safe.** One detector per thread. Never share an `fd_detector *` across threads concurrently. CLIPS environments aren't documented as thread-safe; the conservative reading is "one owner at a time".

- **`fd_observe_player` for *your own color* is silently dropped.** Use `fd_observe_self` for yourself.

- **`fd_reset` does NOT clear the room graph or playstyle.** Intentional: those are game-wide config.

- **`fd_reset` DOES clear closures and the last-sighting cache.** Re-assert any closures that persist across the meeting.

- **`fd_reset` resets the round config to one impostor.** If the next round has 2+ impostors, re-call `fd_observe_round_config` after `fd_reset` — otherwise the round-end demotion fires on the first impostor ejection and the detector goes blind to the rest.

- **`fd_get_suspicion` is a *relative* weight.** It is a per-mille share of a distribution that sums to 1000 across the alive non-self pool — not a standalone P(impostor). A weight of 250 means "a quarter of the suspicion mass", and the same evidence yields a higher weight in a smaller pool. Compare colors within one query; don't compare a weight across rounds or pool sizes.

- **Vent detection has two paths.** *Inferred* — `fd_observe_player` against a `fd_add_room_link` room graph; opt-in, silent until you teach it the map. *Observed* — `fd_observe_vent`, when you directly witness a vent use; needs no graph and is the stronger signal. Both assert `(vent-suspected …)` and yield a `THREAT` stance with `FD_EVIDENCE_VENT`; they differ only in the rationale prose.

- **The vent check compares against shortest-path cost, not direct edges.** Floyd-Warshall finds the cheapest route through what's currently open (respecting closures). A 100-tick direct edge with a 60-tick two-hop alternate is judged by 60.

- **Do not assert an all-pairs room graph.** Assert one link per *physically adjacent* room pair and let shortest-path composition derive the rest. If you instead give every room pair a direct edge — especially costed by straight-line or centroid distance, which dwarfs an actual doorway crossing — every long route collapses to that one overstated edge, ordinary movement looks faster than "possible", and the detector floods with false `(vent-suspected)` facts that turn into bogus `THREAT` stances. This is a *silent* failure: no error, just degraded verdicts. After building the graph, sanity-check it with `fd_room_distance(fd, a, b)` — adjacent rooms should read tens of ticks apart, neighbours-of-neighbours the sum of their hops.

- **Closures are round-scoped.** If the game persists a sabotage across a meeting, re-assert with `fd_observe_route_closed` after `fd_reset`.

- **Queries implicitly run the rule engine.** `fd_pick_vote_target`, `fd_alone_risk`, `fd_render_*`, `fd_get_*`, and the new `fd_room_occupants` / `fd_last_seen` / `fd_rank_suspects` / `fd_round_stats_get` all call `fd_run` internally. Asserts are cheap; queries do the work.

- **First query after a body assertion is the slowest.** The body cascade fires alibi / suspect / stance rules over the accumulated working memory. Known to land in the 10–15 ms range on a heavily-loaded WM. Plan UI/animation between body-discovery and vote.

- **`fd_get_stance` returns `UNKNOWN` for both "never sighted" and "alive, no read yet".** Disambiguate via `fd_get_status` as shown above.

- **Rationale prose is objective.** `fd_get_rationale` and the body of `fd_alone_risk_rationale` describe evidence, not playstyle. Only the risk *label* and the `(paranoid)`/`(trusting)` tag in `fd_alone_risk_rationale` reflect playstyle.

- **`fd_pick_vote_target` returning 0 is normal.** TRUSTING abstains if no `THREAT` exists. NEUTRAL abstains if everyone is `UNKNOWN`. PARANOID abstains only if no alive non-self colors exist.

- **Tie-breaks are by ascending player id.** Id 0 always wins over id 15 within a tier. Determinism, not fairness; if you need fair rotation, layer your own.

- **Room names are caller-defined strings, not validated.** A typo (`"cafateria"`) silently creates a separate room with no edges. Vent checks against it skip. Pick a canonical spelling and stick to it.

- **`fd_profile_*` is diagnostic-only.** Don't ship policy code that depends on its output format. Stable enough to debug with, not stable enough to parse.

- **`docs/model.md` may be stale.** Some early prose there mentions the old Euclidean vent heuristic and the deleted `derive-witness-pair` rule. Trust the headers and this guide for the current shape.

---

## 10. A canonical policy skeleton

A complete (if abbreviated) stub showing the call sequence. Compiles against the public header. The `game_*` calls are placeholders for whatever framework you're driving.

```c
#include "fakedetector.h"
#include <stdio.h>
#include <string.h>

static fd_player my_color(void);
static int      game_tick(void);
static int      game_running(void);
static int      round_over(void);
static int      voting_phase(void);
static int      decision_point(void);
static const char *current_room(void);
static void     cast_vote(fd_player who);
static void     abstain(void);
static void     stay_with_group(void);

int
main(void)
{
    fd_detector *fd = fd_create("clp");
    if (fd == NULL) return 1;

    fd_set_self(fd, my_color());
    fd_set_playstyle(fd, FD_PLAYSTYLE_NEUTRAL);
    fd_observe_round_config(fd, 10, 1);   /* this round's player/impostor count */

    /* Assert the map. Costs are ticks (~25 ticks/sec). */
    fd_add_room_link(fd, "cafeteria", "medbay",  50);
    fd_add_room_link(fd, "cafeteria", "admin",   80);
    fd_add_room_link(fd, "medbay",    "storage", 50);
    fd_add_room_link(fd, "medbay",    "admin",  100);
    /* ... more edges ... */

    while (game_running()) {
        int tick = game_tick();

        /* --- Per tick: feed observations --- */
        fd_observe_self(fd, tick, FD_PHASE_PLAYING,
                        current_room(), 0, 0, /*kill_ready=*/0);
        /* for each visible other player: */
        /* fd_observe_player(fd, tick, who, their_room, their_x, their_y); */
        /* for each newly seen body: */
        /* fd_observe_body(fd, tick, victim, body_room, bx, by); */
        /* for each task completion you witnessed: */
        /* fd_observe_task_completion(fd, tick, who, where); */
        /* sabotage events: */
        /* fd_observe_route_closed(fd, "medbay", "storage"); */

        /* --- Mid-round decisions --- */
        if (decision_point()) {
            fd_player  here[FD_MAX_PLAYERS];
            int n = fd_room_occupants(fd, current_room(), here,
                                      FD_MAX_PLAYERS);
            for (int i = 0; i < n; i++) {
                if (fd_alone_risk(fd, here[i]) >= FD_RISK_HIGH) {
                    stay_with_group();
                    break;
                }
            }
        }

        /* --- Voting phase --- */
        if (voting_phase()) {
            fd_vote_decision_t d;
            fd_vote_decision(fd, &d);
            if (d.recommendation == FD_VOTE_CAST)
                cast_vote(d.target);
            else
                abstain();
        }

        /* --- End of round --- */
        if (round_over()) {
            fd_reset(fd);
            fd_set_self(fd, my_color());        /* always re-set after reset */
            fd_observe_round_config(fd, 10, 1); /* and re-declare the roster */
        }
    }

    fd_destroy(fd);
    return 0;
}
```

---

## 11. What the library deliberately doesn't do

- **No active recommendations.** It tells you stance and risk; it doesn't tell you to "report this body" or "call a meeting now". Build action heuristics on top of the primitives.
- **No per-color trust override.** The playstyle is a global suspicion baseline; there's no API to say "I personally trust YELLOW because of metagame." If you need that, layer it in your policy.
- **No memory across games.** A new `fd_detector` (or a `fd_reset` between rounds) starts with a clean dossier. Metagame state lives in your policy.
- **No vote-aggregation.** Modeling "will my vote actually eject them given how others might vote" is your job. The detector reports its own best target, nothing more.

---

## 12. Telemetry & observability

A `fd_get_*` query gives you the *verdict*. While developing and tuning a policy you also want the *reasoning* behind it. These four calls are stable surface (unlike the diagnostics in §13) — depend on them freely.

### Why is this number what it is — `fd_explain_suspicion`

`fd_get_suspicion` returns a colour's weight; `fd_explain_suspicion` decomposes it into the evidence terms that produced it.

```c
fd_suspicion_explain ex;
if (fd_explain_suspicion(fd, /*yellow*/ 5, &ex) == 1) {
    for (int i = 0; i < ex.n_terms; i++)
        printf("  %-16s %+lld\n", ex.terms[i].source, ex.terms[i].amount);
    printf("  total log-odds %lld -> likelihood %d -> weight %d\n",
           ex.logodds_total, ex.likelihood, ex.weight);
}
```

Each term names an evidence `source` (`vent-observed`, `near-body`, `accusation`, `on-task`, `contradiction`, a `social-*` channel, …) and its signed `amount` in milli-nats — positive is more suspicious, negative is exculpatory. The amounts sum to `logodds_total`. This is how you discover that a channel is over- or under-weighted: if YELLOW reads `570` and the breakdown is all `vent-inferred`, you know the room graph is driving the verdict. Returns `1` filled, `0` for a colour with no belief (self/dead/ejected/never-sighted), `-1` on bad arguments.

### How hard did the engine work — `fd_run_stats`

```c
fd_run_stats rs;
fd_run_stats_get(fd, &rs);   /* does NOT itself fd_run() */
```

`last_rules_fired` / `total_rules_fired` / `run_count`, `last_run_ms` / `max_run_ms`, and `fact_count`. A cheap convergence and latency signal: a quiescent `fd_run` fires `0` rules; a turn that overruns the perf budget shows up in `max_run_ms`. Note this getter deliberately does *not* run the engine — calling it never disturbs `last_rules_fired`.

### What is in working memory — `fd_dump_state`

```c
fd_dump_state(fd, log_fp);   /* any FILE *: a per-round dev log, a pipe */
```

Writes a structured, one-fact-per-line snapshot — dossier, case evidence, social graph, belief, verdict — in a format the library owns (stable, greppable). A superset of `fd_round_stats_get`.

### Capturing a rule-firing trace — `fd_trace_*`

```c
fd_trace_begin(fd, trace_fp);
... fd_run(fd) / any query ...
fd_trace_end(fd);
```

Mirrors CLIPS rule / fact / activation activity to your stream for every `fd_run` in the bracket. Diff the trace between two rule-set versions to see exactly what a change moved. The caller owns the stream; call `fd_trace_end` before closing it.

---

## 13. Diagnostics (for tuning, not policy)

```c
void fd_profile_begin(fd_detector *);
void fd_profile_end(fd_detector *);
void fd_profile_dump(fd_detector *);
void fd_profile_matches(fd_detector *, const char *rule_name);
```

Thin wrappers over CLIPS `(profile constructs)`, `(profile-info)`, and `(matches <rule>)`. Useful when investigating a slow query in the perf harness. **Not** part of the stable API; do not parse the output from policy code. CLIPS profile only measures RHS time; for hotspot analysis use rule-bisection (comment one out, measure) instead.
