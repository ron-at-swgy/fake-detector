;; dossier.clp - maintain one crewmember fact per observed color,
;; track last-seen and visible-task counts, derive near-body evidence,
;; promote crewmembers to dead/ejected on death + ejection signals.

;; ------------------------------------------------------------------
;; Create a crewmember dossier the first time a color is referenced.
;; Two creators (sighting / task-tick) so neither observation type is
;; lost when it arrives before the other.
;; ------------------------------------------------------------------

(defrule ensure-dossier-from-sighting
  (sighting (color ?c))
  (not (crewmember (color ?c)))
  =>
  (assert (crewmember (color ?c) (status alive))))

(defrule ensure-dossier-from-task
  (task-tick (color ?c))
  (not (crewmember (color ?c)))
  =>
  (assert (crewmember (color ?c) (status alive))))

;; ------------------------------------------------------------------
;; Update last-seen when a fresher sighting arrives. The ~dead&~ejected
;; guard prevents stale sightings from resurrecting a known-dead color.
;; The (< ?lt ?t) guard self-disables so this rule cannot loop.
;; ------------------------------------------------------------------

(defrule update-last-seen
  (sighting (color ?c) (tick ?t) (room ?r))
  ?cm <- (crewmember (color ?c)
                     (last-seen-tick ?lt&:(< ?lt ?t))
                     (status ?s&~dead&~ejected))
  =>
  (modify ?cm (last-seen-tick ?t) (last-seen-room ?r) (status alive)))

;; ------------------------------------------------------------------
;; Count visible task completions. Retract the task-tick after it has
;; been credited so the count cannot double-fire.
;; ------------------------------------------------------------------

(defrule count-task
  ?tk <- (task-tick (color ?c))
  ?cm <- (crewmember (color ?c) (visible-tasks ?n))
  =>
  (modify ?cm (visible-tasks (+ ?n 1)))
  (retract ?tk))

;; ------------------------------------------------------------------
;; Body-proximity evidence. A color sighted in the kill room during
;; the case's kill window is "near the body". The window of
;; opportunity has ONE definition -- the kill-window -- rather than a
;; separate hardcoded constant (item 6). The `logical` CE re-derives
;; near-body if the window narrows; the (not near-body) guard adds
;; each (color, room, body-tick) triple at most once.
;;
;; Salience 140: near-body is consumed by the stance layer, so like
;; the cases.clp rules it must settle before stance reads it (see the
;; cases.clp header).
;; ------------------------------------------------------------------

(defrule attach-near-body
  (declare (salience 140))
  (logical (kill-window (victim ?v) (from-tick ?f) (to-tick ?t)
                        (room ?r)))
  (case (victim ?v) (room ?r) (body-tick ?bt))
  (sighting (color ?c) (room ?r) (tick ?st&:(>= ?st ?f)&:(<= ?st ?t)))
  (not (near-body (color ?c) (room ?r) (tick ?bt)))
  =>
  (assert (near-body (color ?c) (room ?r) (tick ?bt))))

;; ------------------------------------------------------------------
;; Death and ejection bookkeeping.
;;
;; body-seen carries the victim's color; a body therefore implies
;; that color is dead. Derive (player-dead) so the mark-dead rule
;; has a single uniform trigger to react to.
;; ------------------------------------------------------------------

(defrule body-implies-death
  (body-seen (color ?c) (tick ?t))
  (not (player-dead (color ?c)))
  =>
  (assert (player-dead (color ?c) (tick ?t))))

;; ------------------------------------------------------------------
;; Ensure a crewmember dossier exists for a known-dead color even if
;; we never sighted them alive — otherwise mark-dead would never fire
;; and fd_get_status would return UNKNOWN for known corpses.
;; ------------------------------------------------------------------

(defrule ensure-dossier-from-death
  (player-dead (color ?c))
  (not (crewmember (color ?c)))
  =>
  (assert (crewmember (color ?c) (status dead))))

;; Promote an existing dossier to dead.
(defrule mark-dead
  (player-dead (color ?c))
  ?cm <- (crewmember (color ?c) (status ?s&~dead&~ejected))
  =>
  (modify ?cm (status dead)))

;; ------------------------------------------------------------------
;; Ejection: bring dossier into existence if absent, then mark ejected.
;; If was-impostor=t, also assert (impostor-found) as a round-end
;; marker; later stance rules can guard on it.
;; ------------------------------------------------------------------

(defrule ensure-dossier-from-ejection
  (ejection (color ?c))
  (not (crewmember (color ?c)))
  =>
  (assert (crewmember (color ?c) (status ejected))))

(defrule mark-ejected
  (ejection (color ?c) (tick ?t) (was-impostor ?w))
  ?cm <- (crewmember (color ?c) (status ?s&~ejected))
  =>
  (modify ?cm (status ejected))
  (if (eq ?w t) then
    (assert (impostor-found (color ?c) (tick ?t)))))
