;; templates.clp - fact schema for fake-detector
;;
;; Three layers:
;;   - raw observations asserted by the C side (self-color, sighting,
;;     body-seen, task-tick, self-state, player-dead, ejection)
;;   - derived per-color dossier (crewmember, near-body)
;;   - verdict consumed at vote time (stance), plus round-end markers
;;     (impostor-found)

;; ------------------------------------------------------------------
;; Identity
;; ------------------------------------------------------------------

;; Singleton-by-convention: the C side asserts/replaces exactly one
;; (self-color) fact per detector instance. Rules can use it to filter
;; self out of opponent-targeted reasoning.
(deftemplate self-color
  (slot color (type SYMBOL)))

;; ------------------------------------------------------------------
;; Raw observations
;; ------------------------------------------------------------------

(deftemplate sighting
  (slot tick  (type INTEGER))
  (slot color (type SYMBOL))
  (slot room  (type SYMBOL))
  (slot x     (type INTEGER))
  (slot y     (type INTEGER)))

;; body-seen carries the dead player's color (the game's snapshot
;; encoder exposes the body's color).
(deftemplate body-seen
  (slot tick  (type INTEGER))
  (slot color (type SYMBOL))
  (slot room  (type SYMBOL))
  (slot x     (type INTEGER))
  (slot y     (type INTEGER)))

(deftemplate task-tick
  (slot tick  (type INTEGER))
  (slot color (type SYMBOL))
  (slot room  (type SYMBOL)))

;; self-state no longer carries self color — that's the singleton
;; (self-color) fact. This template is the per-tick snapshot of the
;; mutable parts of self's state.
(deftemplate self-state
  (slot tick       (type INTEGER))
  (slot phase      (type SYMBOL)
                   (allowed-symbols pregame playing voting results gameover))
  (slot room       (type SYMBOL))
  (slot kill-ready (type SYMBOL) (allowed-symbols t nil) (default nil)))

;; Discrete death event. Either asserted directly via fd_observe_death
;; or derived from a (body-seen) by the body-implies-death rule.
(deftemplate player-dead
  (slot color (type SYMBOL))
  (slot tick  (type INTEGER)))

;; Ejection result with role reveal (the game's player_ejected event).
(deftemplate ejection
  (slot color        (type SYMBOL))
  (slot tick         (type INTEGER))
  (slot was-impostor (type SYMBOL) (allowed-symbols t nil)))

;; ------------------------------------------------------------------
;; Derived dossier
;; ------------------------------------------------------------------

(deftemplate crewmember
  (slot color           (type SYMBOL))
  (slot status          (type SYMBOL)
                        (allowed-symbols alive dead ghost ejected unknown)
                        (default unknown))
  (slot last-seen-tick  (type INTEGER) (default -1))
  (slot last-seen-room  (type SYMBOL)  (default nowhere))
  (slot visible-tasks   (type INTEGER) (default 0)))

;; near-body: this color was sighted in the same room as a discovered
;; body within a Δ-tick window before the body was first seen.
(deftemplate near-body
  (slot color (type SYMBOL))
  (slot room  (type SYMBOL))
  (slot tick  (type INTEGER)))

;; Round-end marker: an ejection revealed an impostor. Future stance
;; rules can guard on this to short-circuit further suspicion.
(deftemplate impostor-found
  (slot color (type SYMBOL))
  (slot tick  (type INTEGER)))

;; ------------------------------------------------------------------
;; Case reasoning (cases.clp) — per-body deductive layer
;; ------------------------------------------------------------------

;; One case per discovered body. Drives all per-kill deduction.
(deftemplate case
  (slot victim    (type SYMBOL))
  (slot room      (type SYMBOL))
  (slot body-tick (type INTEGER)))

;; The murder could have happened in any of these ticks. from-tick is
;; the victim's last sighting alive (or 0 if never sighted); to-tick is
;; the body discovery tick. Room is the body's room.
(deftemplate kill-window
  (slot victim    (type SYMBOL))
  (slot from-tick (type INTEGER))
  (slot to-tick   (type INTEGER))
  (slot room      (type SYMBOL)))

;; A sighting of ?color somewhere other than the kill room during the
;; kill window. On its own this is only a WEAK alibi -- a single
;; glimpse does not prove innocence. derive-cast-iron-alibi promotes it
;; when the room geometry makes the murder physically impossible.
(deftemplate alibi
  (slot color   (type SYMBOL))
  (slot victim  (type SYMBOL))
  (slot at-room (type SYMBOL))
  (slot at-tick (type INTEGER)))

;; A cast-iron alibi: the alibi room is far enough from the kill room
;; (by room-distance) that the sighted color could not have reached the
;; scene within the window. This -- and only this -- clears a suspect.
(deftemplate cast-iron-alibi
  (slot color  (type SYMBOL))
  (slot victim (type SYMBOL)))

;; Alive non-victim non-self color with no CAST-IRON alibi for
;; ?victim's case. Suspect facts shrink as cast-iron alibis arrive.
(deftemplate suspect
  (slot color  (type SYMBOL))
  (slot victim (type SYMBOL))
  (slot basis  (type STRING) (default "no alibi yet")))

;; The deductive climax: a case narrowed to exactly one suspect. The
;; engine names the murderer.
(deftemplate accusation
  (slot victim (type SYMBOL))
  (slot color  (type SYMBOL)))

;; All-pairs shortest-path room costs, in ticks, published by the C
;; library from the caller-asserted navigation graph (see
;; fd_add_room_link). Unreachable pairs carry a sentinel cost larger
;; than any kill window. Consumed by derive-cast-iron-alibi.
(deftemplate room-distance
  (slot from (type SYMBOL))
  (slot to   (type SYMBOL))
  (slot cost (type INTEGER)))

;; Impossible movement / vent use — a strong impostor tell. Asserted
;; directly by the C library, never by a rule, via one of two paths the
;; `basis` slot distinguishes:
;;   inferred -- fd_observe_player saw a move between two rooms that took
;;               fewer ticks than the shortest walking-path cost on the
;;               caller-asserted room graph (see fd_add_room_link).
;;   observed -- fd_observe_vent: the caller directly witnessed the vent
;;               use. from-room and to-room are both the vent's room.
;; `tick` is the absolute tick of the observation (-1 if unknown).
;; Downstream rules split on `basis`: stance-threat-from-vent /
;; strengthen-suspect-vent consume `inferred`, the *-observed-vent
;; siblings consume `observed`.
(deftemplate vent-suspected
  (slot color      (type SYMBOL))
  (slot from-room  (type SYMBOL))
  (slot to-room    (type SYMBOL))
  (slot delta-tick (type INTEGER))
  (slot tick       (type INTEGER) (default -1))
  (slot basis      (type SYMBOL) (allowed-symbols inferred observed)
                   (default inferred)))

;; ------------------------------------------------------------------
;; Testimony / claims layer (clp/claims.clp) and accusation / defense
;; social graph (clp/social.clp). All asserted by the C side from
;; structured chat / vote observations -- the policy does the NLP, the
;; library deduces. Consumed only as logodds-term evidence; they draw
;; no verdict of their own.
;; ------------------------------------------------------------------

;; A claim the policy parsed out of vote-chat. `tick` is the tick the
;; claim is ABOUT (a playing-phase tick the speaker references), not the
;; tick it was uttered -- derive-corroboration and the contradiction
;; distance test depend on that. `room` is the claimed room (location /
;; task) or `nowhere` for a self-report.
(deftemplate claim
  (slot color (type SYMBOL))
  (slot kind  (type SYMBOL) (allowed-symbols location task self-report))
  (slot room  (type SYMBOL))
  (slot tick  (type INTEGER)))

;; A location/task claim refuted by the observation record. Derived in
;; claims.clp; a large positive logodds increment.
(deftemplate contradiction
  (slot color (type SYMBOL))
  (slot basis (type STRING) (default "")))

;; Two colors who each claimed the same room at overlapping ticks with
;; neither claim contradicted -- a mutual weak alibi. Derived in
;; claims.clp; a small negative logodds increment for both.
(deftemplate corroboration
  (slot color-a (type SYMBOL))
  (slot color-b (type SYMBOL))
  (slot room    (type SYMBOL)))

;; `a` accused `b` (a cast vote, or a chat accusation). Asserted by
;; fd_observe_vote.
(deftemplate accuses
  (slot a    (type SYMBOL))
  (slot b    (type SYMBOL))
  (slot tick (type INTEGER)))

;; `a` spoke up in defense of `b`. Asserted by fd_observe_defense.
(deftemplate defends
  (slot a    (type SYMBOL))
  (slot b    (type SYMBOL))
  (slot tick (type INTEGER)))

;; ------------------------------------------------------------------
;; Probabilistic joint-belief layer + round model (clp/suspicion.clp)
;; ------------------------------------------------------------------

;; One immutable contribution fact per evidence instance. CLIPS rules
;; only ever ASSERT these; they are never modified. The C-side pass in
;; fd_run (fd_normalize_suspicion) sums them per color. `amount` is a
;; fixed-point log-likelihood-ratio increment in milli-nat units
;; (1000 = 1.0 nat) -- addition in log space is multiplication of
;; likelihoods, so the rules stay monotone and order-independent.
;; `source` names the evidence channel; `key` disambiguates multiple
;; instances from the same channel (victim color, room+tick, ...) so
;; the once-only (not (logodds-term ...)) guard fires exactly once per
;; (color, source, key) triple.
(deftemplate logodds-term
  (slot color  (type SYMBOL))
  (slot source (type SYMBOL)
               (allowed-symbols prior cast-iron near-body vent-observed
                                vent-inferred accusation off-task on-task
                                contradiction self-report co-location
                                silent-witness social-accuse-impostor
                                social-defend-impostor social-co-accuse
                                social-pack-cleared))
  (slot key    (type STRING)  (default ""))
  (slot amount (type INTEGER) (default 0)))

;; One per alive non-self color. Both numeric slots are written by the
;; C-side pass in fd_run, never by a rule: `logodds` is the summed
;; accumulator, `weight` the normalized per-mille mass (0..1000, sums
;; to 1000 across the alive non-self pool).
(deftemplate suspicion
  (slot color   (type SYMBOL))
  (slot weight  (type INTEGER) (default 0))
  (slot logodds (type INTEGER) (default 0))
  (slot basis   (type STRING)  (default "prior")))

;; Live-count model. n-players / n-impostors are the round config
;; (common knowledge from round start; default to a single impostor
;; when fd_observe_round_config was never called). impostors-ejected,
;; alive-crew and alive-impostors are re-derived each Run pass by
;; roster-recount.
(deftemplate roster
  (slot n-players         (type INTEGER) (default 0))
  (slot n-impostors       (type INTEGER) (default 1))
  (slot alive-crew        (type INTEGER) (default 0))
  (slot alive-impostors   (type INTEGER) (default 1))
  (slot impostors-ejected (type INTEGER) (default 0)))

;; Parity-derived vote urgency, re-derived each Run pass from roster.
(deftemplate game-pressure
  (slot level (type SYMBOL)
              (allowed-symbols low medium high critical)
              (default low)))

;; ------------------------------------------------------------------
;; Verdict
;; ------------------------------------------------------------------

;; evidence names the kind of observation behind the verdict, so a
;; caller can threshold votes on it (e.g. act on near-body / vent but
;; skip an off-task-only accusation). It is orthogonal to category: a
;; THREAT carries accusation / vent / near-body; a non-threat carries
;; off-task / on-task; `none` is a real value — a freshly bootstrapped
;; stance, or a threat demoted after the impostor was found.
(deftemplate stance
  (slot color     (type SYMBOL))
  (slot category  (type SYMBOL)
                  (allowed-symbols on-task off-task threat unknown)
                  (default unknown))
  (slot evidence  (type SYMBOL)
                  (allowed-symbols none accusation vent near-body
                                   off-task on-task)
                  (default none))
  (slot rationale (type STRING) (default "")))

;; ------------------------------------------------------------------
;; Tunable thresholds -- the detective's standards of proof, stated
;; here rather than buried mid-rule.
;; ------------------------------------------------------------------

;; A crewmate is judged on-task once this many task completions have
;; been observed (stance-on-task).
(defglobal ?*on-task-tasks* = 2)

;; Two location/task claims for the same room whose referenced ticks
;; are within this many ticks of each other corroborate one another
;; (derive-corroboration). ~25 ticks/sec, so 50 is ~2 seconds.
(defglobal ?*claim-overlap-window* = 50)
