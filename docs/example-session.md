# Example session: flee, get followed, call the vote

A stylized round showing the loop a policy runs: feed observations, ask
the safety query when it matters, and let the detector argue the case at
the meeting. Every quoted line is real library output from
`bindings/python/examples/flee_and_vote.py` — regenerate it with:

```sh
gmake shared
cd bindings/python/examples
FD_LIBRARY_PATH=../../../build/libfakedetector.so.1 \
PYTHONPATH=.. python3 flee_and_vote.py
```

The cast: we are **red**. Eight players, two impostors. The map is four
rooms with honest walk costs (`cafeteria—admin 60`, `cafeteria—medbay
50`, `medbay—storage 70`, `admin—storage 80`).

## Act 1 — evidence accumulates

Blue grinds tasks in the cafeteria (two watched completions — weak
innocence evidence). Green walks to storage at tick 90. Purple is seen
in **medbay at tick 100**, then in **storage at tick 140** — a 70-tick
walk done in 40. The library's routing check flags it the moment the
sighting lands. At tick 150 we find green's body in storage:

```
purple stance:    THREAT (evidence: VENT)
purple suspicion: 495 / 1000
blue suspicion:   222 / 1000
```

Two deductions interlock here. The case layer opens *the murder of
green* with kill window 90..150; purple's storage sighting at 140 puts
it near the body inside the window. And purple's medbay-at-100 sighting
does **not** earn it a distance alibi — a cast-iron alibi is a
walking-cost argument, and a vent-suspected color has demonstrated the
ability that voids it. (Cyan, seen in admin with no impossible movement,
is legitimately cleared by the same geometry.)

## Act 2 — alone with the threat

Tick 170: we walk into medbay and it's just us and purple. The policy
asks before deciding to stay:

```
alone_risk(purple) = HIGH
  "alone with purple: HIGH risk - impossible movement: medbay to
   storage in 40 ticks (likely vent)"
policy: FLEE toward the button
```

We run for the cafeteria. Tick 230: purple is in the cafeteria too —
`alone_risk` still `HIGH`. That is the button press.

## Act 3 — the meeting

Phase flips to `VOTING`. Purple defends itself: *"I was in admin"* —
a location claim about tick 140. Our own sighting record has purple in
storage at that tick; the claims layer derives a geometric
contradiction, and purple's weight climbs. The belief-aware vote gate:

```
vote decision: CAST -> purple (suspicion 512, confidence 239)
  rationale: "purple is the most likely impostor -- 51% of the
              suspicion, 23 points ahead of the runner-up -- and the
              game is on a knife's edge; vote them out"
```

(The "knife's edge" clause is the parity model talking: the detector has
personally confirmed only four players alive and presumes two impostors
among them, so a wrong skip could lose the game — the evidence bar for
casting drops accordingly.)

What we paste into the vote-chat slot — the summary, then Poirot:

```
fake-detector verdicts (self=red, tick=240, phase=voting):
  purple      threat   impossible movement: medbay to storage in 40 ticks (likely vent)
  pink        off-task alive, no task progress observed
  cyan        off-task alive, no task progress observed
  blue        on-task  observed 2 task completions
  deceased: green(body)

Case file: the murder of green
  Body discovered in storage at tick 150.
  Kill window: ticks 90..150.
  Suspects:
    purple - sighted in storage near body at tick 150
    blue - no alibi yet
    pink - no alibi yet
  Cleared:
    cyan - cast-iron alibi: sighted in admin at tick 140, too far from the scene
```

## What the policy author should notice

- **Three surfaces, one belief state.** `alone_risk` (playing-phase
  safety), `vote_decision` (meeting-time EV gate), and the rendered
  prose all read the same facts; the policy never re-derives anything.
- **The rationale is the API.** The flee decision and the chat argument
  quote the same evidence ("impossible movement: medbay to storage in
  40 ticks"), so the bot's stated reasons are its actual reasons.
- **Claims are ammunition.** Feeding purple's chat defense *raised* its
  suspicion — a geometric contradiction is evidence the accused made
  themselves.
