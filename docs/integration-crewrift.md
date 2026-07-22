# CrewRift integration guide

How a CrewRift policy feeds this library. CrewRift policies receive a
binary sprite stream (Bitworld Sprite-v1) over a websocket — there is no
structured event API on the wire — so every policy already maintains its
own decoder and belief state. The detector sits *downstream of your
decoder*: you translate decoded state into `fd_observe_*` calls and read
decisions back. The mapping code belongs in the policy, not this library.

Identity: CrewRift keys everything by color, and the canonical slot order
is exactly `FD_CREWRIFT_COLORS` in `fd_crewrift.h` (`red, blue, green,
pink, orange, yellow, purple, cyan, lime, brown, beige, navy, teal, rose,
maroon, gray`). Use your decoded color index directly as the `fd_player`
id and call `fd_crewrift_register` (Python: `Detector.register_crewrift`)
once after `fd_create`.

## Observation mapping

The left column uses the vocabulary of a typical decoded-state layer
(e.g. crewborg's `PlayerEvent` kinds and per-tick percepts); the right
column is the library call.

| Policy-side signal | Library call | Notes |
|---|---|---|
| round start | `fd_reset` → `fd_set_self` → `fd_observe_round_config(n, k)` | names persist; re-register only on roster change. Defaults: 8 players, 2 impostors |
| own state per tick | `fd_observe_self(tick, phase, room, x, y, kill_ready)` | coalesce: assert on change, not every tick |
| decoded player sprite in view (`room` / `proximity` events) | `fd_observe_player(tick, id, room, x, y)` | the workhorse; feeds dossier, alibis, vent inference |
| watched task completion (`task` event) | `fd_observe_task_completion(tick, id, room)` | only *verified* completions — fake-tasking is the tell this guards |
| body sprite decoded (`near_body` context) | `fd_observe_body(tick, victim, room, x, y)` | do NOT pre-digest proximity: the library derives `near-body` itself from the sighting record |
| witnessed kill | `fd_observe_body` + the killer's `fd_observe_player` at the scene | no direct "X killed Y" channel yet — see Gaps |
| vent use witnessed (`vent_use` event) | `fd_observe_vent(tick, id, room)` + the paired `fd_observe_player` sighting | the sighting is required — a vent fact for a never-sighted id sits inert |
| player vanished from census | `fd_observe_death(tick, id)` | for deaths inferred from the voting-grid census rather than a body |
| vote-grid dots decoded | `fd_observe_vote(tick, voter, target)` per dot | feeds the social accuse graph |
| ejection result + role reveal | `fd_observe_ejection(tick, who, was_impostor)` | triggers the social layer's retro-scoring on reveal |
| chat line, parsed by your NLP: location/task claim | `fd_observe_claim(tick, speaker, kind, room)` | `tick` is the playing-phase tick the claim *refers to*, not the utterance tick |
| chat: "I found the body" | `fd_observe_claim(..., FD_CLAIM_SELF_REPORT, room)` | documented impostor tell |
| chat: X defends Y | `fd_observe_defense(tick, x, y)` | inert until a reveal retro-scores it |
| chat: X accuses Y | `fd_observe_vote(tick, x, y)` | accusations and votes share the `accuses` edge |

Decisions out: `fd_vote_decision` (belief-aware SKIP/ABSTAIN/CAST — prefer
this), `fd_rank_suspects`, `fd_alone_risk` (playing-phase safety),
`fd_render_vote_summary` / `fd_render_case` (prose for the chat slot),
`fd_explain_suspicion` (per-term attribution while tuning).

## The room graph

CrewRift's map is pixel/tile-based; the library wants a *sparse doorway
adjacency graph* with honest minimum traversal costs in ticks. Your policy
already has (or wants) a zone/room abstraction over the map — walk its
zone adjacency once at startup and call
`fd_add_room_link(zoneA, zoneB, min_walk_ticks)` per adjacent pair.

Two hard rules, both failure modes the library cannot detect for you:

- **One link per adjacent pair only — never all-pairs.** The library runs
  shortest-paths itself; an all-pairs "graph" makes every move look legal
  or, with short costs, floods false vent suspicions.
- **Costs are the *minimum honest* walk time.** Optimistic costs create
  false vents; pessimistic ones miss real vents. Verify with
  `fd_room_distance(a, b)` against hand-timed walks.

Skip the graph entirely to disable inferred-vent detection — directly
observed vents (`fd_observe_vent`) still work without it.

## Gaps (documented, not blocking)

- **No "X killed Y" channel.** A policy that witnesses a kill can only
  assert the body + the killer's presence; near-body evidence then fires.
  A first-class `fd_observe_kill_witnessed` is a candidate API once
  shadow-mode data shows the near-body path underweights witnessed kills.
- **No tailing/shadowing channel.** "This player has been following me"
  stays policy-side.
- **Reporter identity.** The observation surface doesn't currently expose
  who reported a body; there is nothing to assert (see docs/model.md,
  "deliberately not modeled").

## Proving the integration

Run the detector shadow-mode inside your policy first: feed it everything,
log `fd_rank_suspects` / `fd_vote_decision` next to your native suspicion
logic each meeting, and diff the two before letting it vote. The
divergence log is also exactly what `fd_explain_suspicion` is for.
