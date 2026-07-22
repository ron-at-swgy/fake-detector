;; cases.clp - per-body deductive layer (Sherlock/Poirot logic).
;;
;; The chain on a body discovery:
;;   body-seen -> open-case -> init-kill-window -> update-kill-window-lower*
;;     -> derive-alibi* -> derive-cast-iron-alibi*
;;     -> open-suspect-from-crewmember* / retract-suspect-on-cast-iron-alibi*
;;     -> solve-case
;;
;; Salience: this file uses ONE band -- every rule here sits at
;; 130..150, above the stance layer (clp/stance.clp, whose promotion
;; rules top out at 120). That is the single load-bearing ordering
;; decision: the case layer must fully settle before any stance rule
;; reads its conclusions (cast-iron-alibi, accusation, near-body),
;; otherwise a high-salience stance-threat could fire against a
;; not-yet-derived alibi and wrongly flag a cleared suspect. WITHIN
;; the band, ordering is by fact dependency, not salience -- a rule
;; cannot reach the agenda before the fact its predecessor asserts
;; exists. Two intra-band exceptions are flagged at their rules:
;; update-kill-window-lower (150) above derive-alibi, and the
;; strengthen-suspect-* rules above weaken-suspect-on-weak-alibi.
;;
;; Truth maintenance (the `logical` CEs on derive-alibi /
;; derive-cast-iron-alibi / solve-case, and on attach-near-body in
;; dossier.clp) keeps conclusions consistent when evidence shifts
;; across Run() passes: when update-kill-window-lower narrows the
;; window, dependent alibis and accusations auto-retract and re-derive.
;;
;; Impossible-movement (vent) detection is done in the C library, which
;; asserts (vent-suspected ...) facts directly. See fd_add_room_link.

;; ------------------------------------------------------------------
;; Case opening -- every body becomes a case.
;; ------------------------------------------------------------------

(defrule open-case
  (declare (salience 140))
  (body-seen (color ?v) (room ?r) (tick ?bt))
  (not (case (victim ?v)))
  =>
  (assert (case (victim ?v) (room ?r) (body-tick ?bt))))

;; ------------------------------------------------------------------
;; Kill-window derivation. Initialize with from-tick=0; ratchet up to
;; the latest pre-body sighting of the victim as evidence accumulates.
;; ------------------------------------------------------------------

(defrule init-kill-window
  (declare (salience 140))
  (case (victim ?v) (room ?r) (body-tick ?bt))
  (not (kill-window (victim ?v)))
  =>
  (assert (kill-window (victim ?v) (from-tick 0) (to-tick ?bt) (room ?r))))

;; Salience 150: one notch above the rest of the band so that, within
;; a Run() pass, the window is narrowed before alibis are derived
;; against it -- avoiding needless derive/retract churn. Correctness
;; does not depend on it (the `logical` CE on derive-alibi re-derives
;; if the window moves later); this is a churn-reduction optimization.
(defrule update-kill-window-lower
  (declare (salience 150))
  (sighting (color ?v) (tick ?st))
  ?kw <- (kill-window (victim ?v)
                      (from-tick ?ka&:(< ?ka ?st))
                      (to-tick   ?bt&:(> ?bt ?st)))
  =>
  (modify ?kw (from-tick ?st)))

;; ------------------------------------------------------------------
;; Weak alibi. A sighting of a non-victim color in a room other than
;; the kill room, during the kill window. The `logical` CE makes the
;; alibi depend on the kill-window: when update-kill-window-lower
;; narrows the window, this alibi auto-retracts and re-derives against
;; the new bounds -- a sighting that no longer falls in the window
;; stops being an alibi. (Item 1: the detective revises.)
;; ------------------------------------------------------------------

(defrule derive-alibi
  (declare (salience 140))
  (logical (kill-window (victim ?v) (from-tick ?f) (to-tick ?t)
                        (room ?k)))
  (sighting (color ?c&~?v) (room ?r&~?k)
            (tick ?st&:(>= ?st ?f)&:(<= ?st ?t)))
  (not (alibi (color ?c) (victim ?v)))
  =>
  (assert (alibi (color ?c) (victim ?v) (at-room ?r) (at-tick ?st))))

;; ------------------------------------------------------------------
;; Cast-iron alibi. A weak alibi becomes cast-iron only when the room
;; geometry makes the murder physically impossible: the sighted color,
;; seen in room ?r at tick ?st, could not have reached the kill room
;; ?k and been at the scene during the window. With dist = the
;; shortest-path cost ?r<->?k, that holds iff dist exceeds the time
;; available on *both* sides of the sighting: dist > (st - f) and
;; dist > (t - st). Only a cast-iron alibi clears a suspect. (Item 2.)
;; ------------------------------------------------------------------

;; A cast-iron alibi is a walking-cost argument: "?c could not have
;; legally reached the scene". A color with a vent-suspected fact has
;; demonstrated exactly the ability that voids that argument, so no
;; room-distance alibi can clear it. The (not ...) sits INSIDE the
;; logical block: a vent observed after the alibi derived retracts the
;; alibi (and, through term-cast-iron's logical CE, its -8000 term).
(defrule derive-cast-iron-alibi
  (declare (salience 140))
  (logical
    (kill-window (victim ?v) (from-tick ?f) (to-tick ?t) (room ?k))
    (alibi (color ?c) (victim ?v) (at-room ?r) (at-tick ?st))
    (room-distance (from ?r) (to ?k) (cost ?d))
    (not (vent-suspected (color ?c))))
  (test (and (> ?d (- ?st ?f)) (> ?d (- ?t ?st))))
  (not (cast-iron-alibi (color ?c) (victim ?v)))
  =>
  (assert (cast-iron-alibi (color ?c) (victim ?v))))

;; ------------------------------------------------------------------
;; Suspect pool. Every alive non-self non-victim crewmember is a
;; suspect for each open case until a CAST-IRON alibi clears them.
;; A weak alibi does NOT remove a suspect (see weaken-suspect-on-weak-
;; alibi). The (not suspect) / (not cast-iron-alibi) guards break the
;; oscillation with retract-suspect-on-cast-iron-alibi.
;; ------------------------------------------------------------------

(defrule open-suspect-from-crewmember
  (declare (salience 140))
  (case (victim ?v))
  (self-color (color ?self))
  (crewmember (color ?c&~?v&~?self) (status alive))
  (not (cast-iron-alibi (color ?c) (victim ?v)))
  (not (suspect (color ?c) (victim ?v)))
  =>
  (assert (suspect (color ?c) (victim ?v))))

(defrule retract-suspect-on-cast-iron-alibi
  (declare (salience 140))
  (cast-iron-alibi (color ?c) (victim ?v))
  ?s <- (suspect (color ?c) (victim ?v))
  =>
  (retract ?s))

;; ------------------------------------------------------------------
;; Suspect-basis enrichment. Replace the default "no alibi yet" with
;; the strongest concrete reason this color is still suspected. Each
;; rule matches only the default placeholder, so it self-disables once
;; the basis is set.
;;
;; Salience: the incriminating reasons (near-body, vent) at 140 outrank
;; the exculpatory note (a weak alibi) at 130. A suspect who is both
;; near the body and partially alibied reads as "near body", not
;; "partially alibied".
;; ------------------------------------------------------------------

(defrule strengthen-suspect-near-body
  (declare (salience 140))
  ?s <- (suspect (color ?c) (victim ?v)
                 (basis ?cur&:(eq ?cur "no alibi yet")))
  (near-body (color ?c) (room ?r) (tick ?bt))
  (case (victim ?v) (room ?r) (body-tick ?bt))
  =>
  (modify ?s (basis (str-cat "sighted in " ?r
                             " near body at tick " ?bt))))

(defrule strengthen-suspect-vent
  (declare (salience 140))
  ?s <- (suspect (color ?c) (victim ?v)
                 (basis ?cur&:(eq ?cur "no alibi yet")))
  (vent-suspected (color ?c) (from-room ?r1) (to-room ?r2)
                  (basis inferred))
  =>
  (modify ?s (basis (str-cat "vented from " ?r1 " to " ?r2))))

(defrule strengthen-suspect-observed-vent
  (declare (salience 140))
  ?s <- (suspect (color ?c) (victim ?v)
                 (basis ?cur&:(eq ?cur "no alibi yet")))
  (vent-suspected (color ?c) (from-room ?r) (basis observed))
  =>
  (modify ?s (basis (str-cat "seen using a vent in " ?r))))

(defrule weaken-suspect-on-weak-alibi
  (declare (salience 130))
  ?s <- (suspect (color ?c) (victim ?v)
                 (basis ?cur&:(eq ?cur "no alibi yet")))
  (alibi (color ?c) (victim ?v) (at-room ?r) (at-tick ?st))
  (not (cast-iron-alibi (color ?c) (victim ?v)))
  =>
  (modify ?s (basis (str-cat "partially alibied -- seen in " ?r
                             " at tick " ?st ", but not cleared"))))

;; ------------------------------------------------------------------
;; The reveal. A case narrowed to exactly one suspect is solved: that
;; color is the murderer. The `logical` CE ties the accusation to the
;; lone-suspect state -- if a second suspect re-appears (an alibi was
;; invalidated) or the sole suspect is cleared, the accusation
;; auto-retracts. (Item 3: "...whatever remains is the truth.")
;; ------------------------------------------------------------------

(defrule solve-case
  (declare (salience 140))
  (logical
    (case (victim ?v))
    (suspect (color ?c) (victim ?v))
    (not (suspect (color ?c2&~?c) (victim ?v))))
  (not (accusation (victim ?v)))
  =>
  (assert (accusation (victim ?v) (color ?c))))
