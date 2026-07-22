;; claims.clp - testimony layer: turn parsed vote-chat claims into
;; deductive signal (Enhancement 3, plus the silent-witness signal of
;; Enhancement 4).
;;
;; The C side asserts (claim ...) facts from chat the policy has already
;; parsed -- the policy does the NLP, the library deduces. This file
;; derives two intermediate facts and emits logodds-term increments:
;;
;;   contradiction  - a location/task claim refuted by the sighting
;;                    record. Geometric, not behavioural -- it cannot be
;;                    explained away. Large positive increment.
;;   corroboration  - two colors who vouched for the same room at
;;                    overlapping ticks, neither contradicted. A mutual
;;                    weak alibi; small negative increment for both.
;;
;; Salience: derivation rules at 140 (the cases.clp band, so the facts
;; settle before the term band); term rules at 90 (the suspicion.clp
;; term band). Everything here is opt-in -- with no (claim) facts fed,
;; no rule fires.

;; ------------------------------------------------------------------
;; Contradiction derivation. Two paths, both guarded so a color is
;; contradicted at most once.
;; ------------------------------------------------------------------

;; Graph-free path: the color claimed room ?cr at tick ?t but a sighting
;; places them in a different room at the SAME tick. Nobody is in two
;; rooms at once -- no room graph needed.
(defrule derive-contradiction-colocation
  (declare (salience 140))
  (claim (color ?c) (kind ~self-report) (room ?cr) (tick ?t))
  (sighting (color ?c) (room ?sr&~?cr) (tick ?t))
  (not (contradiction (color ?c)))
  =>
  (assert (contradiction (color ?c)
    (basis (str-cat "claimed " ?cr " at tick " ?t
                    " but was sighted in " ?sr)))))

;; Routing path: the claimed room is unreachable from a confirmed
;; sighting within the elapsed ticks. Reuses the room-distance machinery
;; that powers derive-cast-iron-alibi; no-ops when no room graph was
;; asserted (no room-distance facts), so this path is opt-in.
(defrule derive-contradiction-by-distance
  (declare (salience 140))
  (claim (color ?c) (kind ~self-report) (room ?cr) (tick ?ct))
  (sighting (color ?c) (room ?sr&~?cr) (tick ?st))
  (room-distance (from ?cr) (to ?sr) (cost ?d))
  (test (> ?d (abs (- ?st ?ct))))
  (not (contradiction (color ?c)))
  =>
  (assert (contradiction (color ?c)
    (basis (str-cat "claimed " ?cr " at tick " ?ct
                    " -- too far from " ?sr " at tick " ?st)))))

;; ------------------------------------------------------------------
;; Corroboration. Two colors who each claimed the same room within the
;; overlap window, with NEITHER contradicted, vouch for one another.
;; The (not (contradiction ...)) CEs sit inside the `logical` block, so
;; a contradiction derived later retracts the corroboration (and its
;; co-location terms). The dual (not (corroboration ...)) self-guards
;; yield exactly one fact per unordered pair.
;; ------------------------------------------------------------------

(defrule derive-corroboration
  (declare (salience 140))
  (logical
    (claim (color ?a) (kind ~self-report) (room ?r) (tick ?ta))
    (claim (color ?b&~?a) (kind ~self-report) (room ?r) (tick ?tb))
    (not (contradiction (color ?a)))
    (not (contradiction (color ?b))))
  (test (< (abs (- ?ta ?tb)) ?*claim-overlap-window*))
  (not (corroboration (color-a ?a) (color-b ?b)))
  (not (corroboration (color-a ?b) (color-b ?a)))
  =>
  (assert (corroboration (color-a ?a) (color-b ?b) (room ?r))))

;; ------------------------------------------------------------------
;; logodds-term emission (salience 90, the suspicion.clp term band).
;; Amounts are milli-nats (1000 = 1.0 nat); tunable.
;; ------------------------------------------------------------------

;; A refuted claim -- geometric, cannot be explained away. One term per
;; color (key ""); `contradiction` is never retracted, so not `logical`.
(defrule term-contradiction
  (declare (salience 90))
  (contradiction (color ?c))
  (not (logodds-term (color ?c) (source contradiction) (key "")))
  =>
  (assert (logodds-term (color ?c) (source contradiction)
                        (key "") (amount 2500))))

;; Self-report ("I found the body") is a documented impostor tactic --
;; a small tell, never decisive on its own.
(defrule term-self-report
  (declare (salience 90))
  (claim (color ?c) (kind self-report))
  (not (logodds-term (color ?c) (source self-report) (key "")))
  =>
  (assert (logodds-term (color ?c) (source self-report)
                        (key "") (amount 400))))

;; Corroboration is a mutual weak alibi: a small negative increment for
;; BOTH colors. Two sibling rules, each keyed by the partner color, so a
;; color corroborated by several partners accrues one term per partner
;; and neither color is double-counted.
(defrule term-co-location-a
  (declare (salience 90))
  (logical (corroboration (color-a ?a) (color-b ?b)))
  (not (logodds-term (color ?a) (source co-location) (key =(str-cat ?b))))
  =>
  (assert (logodds-term (color ?a) (source co-location)
                        (key (str-cat ?b)) (amount -400))))

(defrule term-co-location-b
  (declare (salience 90))
  (logical (corroboration (color-a ?a) (color-b ?b)))
  (not (logodds-term (color ?b) (source co-location) (key =(str-cat ?a))))
  =>
  (assert (logodds-term (color ?b) (source co-location)
                        (key (str-cat ?a)) (amount -400))))

;; Silent witness (Enh. 4): a color whose sighting history places them
;; at a kill scene during the window (a near-body fact) yet who files no
;; self-report and no location claim naming the kill room -- an innocent
;; witness would normally speak up. Gated on (exists (claim)): with the
;; claims channel unused, silence is not informative, so the rule stays
;; inert. All four dependency CEs are in one `logical` block (`case`
;; first, to bind ?v/?r/?bt) so a later claim retracts the term. A
;; *contradicted* claim is already penalised by term-contradiction;
;; this only covers genuine silence.
(defrule term-silent-witness
  (declare (salience 90))
  (logical
    (case (victim ?v) (room ?r) (body-tick ?bt))
    (near-body (color ?c) (room ?r) (tick ?bt))
    (not (claim (color ?c) (kind self-report)))
    (not (claim (color ?c) (kind location) (room ?r))))
  (exists (claim))
  (not (logodds-term (color ?c) (source silent-witness)
                     (key =(str-cat ?v))))
  =>
  (assert (logodds-term (color ?c) (source silent-witness)
                        (key (str-cat ?v)) (amount 700))))
