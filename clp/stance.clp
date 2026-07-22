;; stance.clp - derive a single stance per ALIVE crewmember.
;;
;; Salience order ensures the worst verdict that applies wins:
;;   impostor-clear (250) > status-retract (200) > accuse (120)
;;   > threat-vent (110) > threat-case (100) > off-task (20)
;;   > on-task (10) > unknown (-10)
;;
;; Each rule's pattern excludes its own category and any higher-priority
;; category so it cannot self-fire and cannot overwrite a worse verdict.
;; Stance rules require (status alive) — dead/ejected colors have no
;; stance fact and are excluded from fd_pick_vote_target.
;;
;; The case-derivation layer (clp/cases.clp and attach-near-body in
;; clp/dossier.clp) runs at salience 130..150 — above every rule here —
;; so the facts the stance rules read (cast-iron-alibi, accusation,
;; near-body) are fully settled before any verdict is drawn.

;; ------------------------------------------------------------------
;; Retract a stance whose subject is no longer alive. Higher salience
;; than any stance-promotion rule, so a dead color's verdict disappears
;; before any other rule reads it.
;; ------------------------------------------------------------------

(defrule retract-stance-on-status-change
  (declare (salience 200))
  (crewmember (color ?c) (status ?s&~alive))
  ?st <- (stance (color ?c))
  =>
  (retract ?st))

;; ------------------------------------------------------------------
;; Round-end demotion: once EVERY impostor has been ejected, remaining
;; alive crewmates can't be the killer. The gate is roster-driven --
;; impostors-ejected >= n-impostors -- so a multi-impostor round is not
;; cleared by the first ejection (Enh. 2). With the default n-impostors
;; of 1 this reduces exactly to the single-impostor behavior. Demote
;; any lingering threat verdict to unknown so vote-pick stops accusing
;; the cleared.
;; ------------------------------------------------------------------

(defrule clear-threats-on-impostor-found
  (declare (salience 250))
  (roster (n-impostors ?ni) (impostors-ejected ?ne&:(>= ?ne ?ni)))
  ?st <- (stance (color ?c) (category threat))
  =>
  (modify ?st (category unknown)
              (evidence none)
              (rationale "round-end: all impostors ejected")))

;; ------------------------------------------------------------------
;; Bootstrap: every alive dossiered crewmember gets a stance fact.
;; Default category is `unknown` from the deftemplate.
;; ------------------------------------------------------------------

(defrule stance-ensure
  (declare (salience -10))
  (crewmember (color ?c) (status alive))
  (not (stance (color ?c)))
  =>
  (assert (stance (color ?c))))

;; ------------------------------------------------------------------
;; ACCUSED: a case has collapsed to exactly one suspect (cases.clp's
;; solve-case asserted an accusation). This is the deductive climax --
;; it outranks every other threat signal so the named murderer's
;; rationale survives. The ~threat guard plus this rule's salience mean
;; the lower threat rules see category=threat and skip, leaving the
;; accusation rationale in place.
;; ------------------------------------------------------------------

(defrule stance-accuse
  (declare (salience 120))
  (accusation (victim ?v) (color ?c))
  (crewmember (color ?c) (status alive))
  (not (roster (n-impostors ?ni1) (impostors-ejected ?ne1&:(>= ?ne1 ?ni1))))
  ?st <- (stance (color ?c) (category ?cat&~threat))
  =>
  (modify ?st (category threat)
              (evidence accusation)
              (rationale (str-cat "the case against " ?c
                                  " is closed: sole suspect in the "
                                  "murder of " ?v))))

;; ------------------------------------------------------------------
;; THREAT (vent): impossible-movement detection. Strongest single
;; signal — overrides any other stance below. The two rules split on
;; the vent-suspected `basis` slot: an inferred vent (routing-distance
;; deduction) and a directly observed one differ only in their prose.
;; ------------------------------------------------------------------

(defrule stance-threat-from-vent
  (declare (salience 110))
  (vent-suspected (color ?c) (from-room ?r1) (to-room ?r2)
                  (delta-tick ?d) (basis inferred))
  (crewmember (color ?c) (status alive))
  (not (roster (n-impostors ?ni2) (impostors-ejected ?ne2&:(>= ?ne2 ?ni2))))
  ?st <- (stance (color ?c) (category ?cat&~threat))
  =>
  (modify ?st (category threat)
              (evidence vent)
              (rationale (str-cat "impossible movement: " ?r1 " to " ?r2
                                  " in " ?d " ticks (likely vent)"))))

(defrule stance-threat-from-observed-vent
  (declare (salience 110))
  (vent-suspected (color ?c) (from-room ?r) (tick ?t) (basis observed))
  (crewmember (color ?c) (status alive))
  (not (roster (n-impostors ?ni3) (impostors-ejected ?ne3&:(>= ?ne3 ?ni3))))
  ?st <- (stance (color ?c) (category ?cat&~threat))
  =>
  (modify ?st (category threat)
              (evidence vent)
              (rationale (str-cat "seen using a vent in " ?r
                                  " at tick " ?t))))

;; ------------------------------------------------------------------
;; THREAT (case-based): sighted near a discovered body during its
;; case's kill window AND no CAST-IRON alibi for that case. The alibi
;; gate implements Sherlock's "eliminate the impossible" — a near-body
;; color is cleared only by an alibi the room geometry makes
;; unbreakable, not by a single glimpse elsewhere (see cases.clp's
;; derive-cast-iron-alibi). Body-report-based exoneration is deferred —
;; Among Them does not currently surface reporter identity for other
;; players (upstream Sprint 2.4 TODO). See docs/model.md.
;; ------------------------------------------------------------------

(defrule stance-threat
  (declare (salience 100))
  (near-body (color ?c) (room ?r) (tick ?bt))
  (crewmember (color ?c) (status alive))
  (case (victim ?v) (room ?r) (body-tick ?bt))
  (not (cast-iron-alibi (color ?c) (victim ?v)))
  (not (roster (n-impostors ?ni4) (impostors-ejected ?ne4&:(>= ?ne4 ?ni4))))
  ?st <- (stance (color ?c) (category ?cat&~threat))
  =>
  (modify ?st (category threat)
              (evidence near-body)
              (rationale (str-cat "sighted in " ?r " at tick " ?bt
                                  " near body; no alibi"))))

;; ------------------------------------------------------------------
;; OFF-TASK: alive, dossiered, but zero observed task completions.
;; The ~threat guard means stance-threat already had its chance to fire.
;; ------------------------------------------------------------------

(defrule stance-off-task
  (declare (salience 20))
  (crewmember (color ?c) (status alive) (visible-tasks 0))
  ?st <- (stance (color ?c) (category ?cat&~off-task&~threat))
  =>
  (modify ?st (category off-task)
              (evidence off-task)
              (rationale "alive, no task progress observed")))

;; ------------------------------------------------------------------
;; ON-TASK: at least ?*on-task-tasks* visible task completions.
;; Salience-priority means a threat verdict already took precedence; a
;; prior off-task verdict is promoted if task progress accumulates.
;; ------------------------------------------------------------------

(defrule stance-on-task
  (declare (salience 10))
  (crewmember (color ?c) (status alive)
              (visible-tasks ?n&:(>= ?n ?*on-task-tasks*)))
  ?st <- (stance (color ?c) (category ?cat&~on-task&~threat))
  =>
  (modify ?st (category on-task)
              (evidence on-task)
              (rationale (str-cat "observed " ?n " task completions"))))
