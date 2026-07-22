;; suspicion.clp - probabilistic joint-belief substrate + round model.
;;
;; This file does NOT draw a verdict. It maintains three fact families
;; that the C side and the stance layer consume:
;;
;;   roster         - live (crew, impostor) counts; the multi-impostor
;;                    correctness model (Enh. 2).
;;   game-pressure  - parity-derived vote urgency, from the roster.
;;   logodds-term   - immutable per-(color,source,key) evidence
;;                    contributions in log-odds space (Enh. 1). The
;;                    C-side pass fd_normalize_suspicion sums these into
;;                    each (suspicion) fact's `logodds`/`weight` slots.
;;
;; Loaded between cases.clp and stance.clp. Salience bands:
;;   160  roster maintenance  - above the case band so counts are
;;                              current whenever anything reads them.
;;   155  game-pressure       - derived straight from the roster.
;;   200  retract-suspicion   - mirrors retract-stance-on-status-change.
;;    95  suspicion-ensure / prior - bootstrap, below the stance band.
;;    90  evidence -> logodds-term - runs after the case layer (130..140)
;;                              has fully settled cast-iron-alibi /
;;                              accusation / near-body.
;;
;; Idempotency: every rule here either asserts an IMMUTABLE fact exactly
;; once (guarded by a (not ...) on the fact it would assert) or modifies
;; a singleton only when a computed value actually changed. So repeated
;; Run() passes -- and every fd_run is one -- converge to the same WM.

;; ------------------------------------------------------------------
;; Roster: the live-count model.
;; ------------------------------------------------------------------

;; Create the singleton roster if absent. n-impostors defaults to 1 --
;; the single-impostor assumption when fd_observe_round_config was not
;; called. fd_observe_round_config replaces this fact wholesale.
(defrule roster-ensure
  (declare (salience 160))
  (not (roster))
  =>
  (assert (roster)))

;; Recompute the derived counts whenever a crewmember fact changes
;; (which also covers every ejection, since mark-ejected modifies the
;; crewmember). impostors-ejected counts ejection facts revealed as
;; impostors; alive-impostors is n-impostors minus that, floored at 0;
;; alive-crew is the remaining alive non-self colors. The (or (<> ...))
;; guard means a modify happens only on a real change, so the rule
;; cannot self-loop and is stable across repeated Run() passes.
(defrule roster-recount
  (declare (salience 160))
  (crewmember)
  ?r <- (roster (n-impostors ?ni)
                (alive-crew ?oc) (alive-impostors ?oi)
                (impostors-ejected ?oe))
  =>
  (bind ?ejected 0)
  (do-for-all-facts ((?e ejection)) (eq ?e:was-impostor t)
    (bind ?ejected (+ ?ejected 1)))
  (bind ?alive 0)
  (do-for-all-facts ((?cm crewmember)) (eq ?cm:status alive)
    (bind ?alive (+ ?alive 1)))
  (bind ?ai (max 0 (- ?ni ?ejected)))
  (bind ?ac (max 0 (- ?alive ?ai)))
  (if (or (<> ?ejected ?oe) (<> ?ac ?oc) (<> ?ai ?oi)) then
    (modify ?r (impostors-ejected ?ejected)
               (alive-crew ?ac) (alive-impostors ?ai))))

;; ------------------------------------------------------------------
;; Game-pressure: parity-aware vote urgency derived from the roster.
;; surplus = alive-crew - alive-impostors; crew lose at surplus <= 0.
;; An ODD surplus reads one band hotter than the even surplus above it
;; (Migdal 1009.1031: adding one townsperson can help the mafia).
;; ------------------------------------------------------------------

(defrule pressure-ensure
  (declare (salience 155))
  (not (game-pressure))
  =>
  (assert (game-pressure)))

(defrule pressure-derive
  (declare (salience 155))
  (roster (alive-crew ?ac) (alive-impostors ?ai))
  ?gp <- (game-pressure (level ?old))
  =>
  (bind ?s (- ?ac ?ai))
  (bind ?lvl (if (<= ?s 1) then critical
              else (if (<= ?s 2) then high
              else (if (<= ?s 4) then medium
              else low))))
  (if (and (> ?s 1) (= (mod ?s 2) 1)) then
    (bind ?lvl (if (eq ?lvl medium) then high
                else (if (eq ?lvl low) then medium else ?lvl))))
  (if (neq ?lvl ?old) then (modify ?gp (level ?lvl))))

;; ------------------------------------------------------------------
;; suspicion facts: one per alive non-self color. CLIPS only ever
;; CREATES and RETRACTS them here; the numeric slots (weight, logodds)
;; are filled by the C-side pass fd_normalize_suspicion. Retracting a
;; dead/ejected color's suspicion is what makes belief mass redistribute
;; over the survivors (DeepRole "evidence about one updates all").
;; ------------------------------------------------------------------

(defrule suspicion-ensure
  (declare (salience 95))
  (self-color (color ?self))
  (crewmember (color ?c&~?self) (status alive))
  (not (suspicion (color ?c)))
  =>
  (assert (suspicion (color ?c))))

(defrule retract-suspicion-on-status-change
  (declare (salience 200))
  (crewmember (color ?c) (status ?s&~alive))
  ?su <- (suspicion (color ?c))
  =>
  (retract ?su))

;; ------------------------------------------------------------------
;; logodds-term: immutable per-(color,source,key) evidence increments.
;; Each rule fires exactly once per evidence instance -- the
;; (not (logodds-term ...)) guard self-disables it. Most are wrapped in
;; a `logical` CE so the term auto-retracts if its evidence is retracted
;; by truth maintenance (e.g. a kill-window narrowing drops a near-body).
;; Amounts are milli-nats (1000 = 1.0 nat); they are tunable.
;; ------------------------------------------------------------------

;; Uninformed prior: a zero-amount term per alive non-self color. A flat
;; prior cancels under normalization, so this exists only to guarantee
;; every live color has at least one term; the informative m/n value is
;; applied C-side as the vote threshold, not as a per-color offset.
(defrule suspicion-prior
  (declare (salience 95))
  (self-color (color ?self))
  (crewmember (color ?c&~?self) (status alive))
  (not (logodds-term (color ?c) (source prior)))
  =>
  (assert (logodds-term (color ?c) (source prior) (key "") (amount 0))))

;; Cast-iron alibi: mass -> ~0. A large negative floor, one per case.
(defrule term-cast-iron
  (declare (salience 90))
  (logical (cast-iron-alibi (color ?c) (victim ?v)))
  (not (logodds-term (color ?c) (source cast-iron) (key =(str-cat ?v))))
  =>
  (assert (logodds-term (color ?c) (source cast-iron)
                        (key (str-cat ?v)) (amount -8000))))

;; Directly observed vent use: near-certain impostor tell.
(defrule term-vent-observed
  (declare (salience 90))
  (logical (vent-suspected (color ?c) (basis observed)
                           (from-room ?r) (tick ?t)))
  (not (logodds-term (color ?c) (source vent-observed)
                     (key =(str-cat ?r "-" ?t))))
  =>
  (assert (logodds-term (color ?c) (source vent-observed)
                        (key (str-cat ?r "-" ?t)) (amount 3000))))

;; Inferred (routing-distance) vent: strong, not certain.
(defrule term-vent-inferred
  (declare (salience 90))
  (logical (vent-suspected (color ?c) (basis inferred)
                           (from-room ?r1) (to-room ?r2) (tick ?t)))
  (not (logodds-term (color ?c) (source vent-inferred)
                     (key =(str-cat ?r1 "-" ?r2 "-" ?t))))
  =>
  (assert (logodds-term (color ?c) (source vent-inferred)
                        (key (str-cat ?r1 "-" ?r2 "-" ?t)) (amount 2000))))

;; Accusation (a case solved against this color): moderate positive.
(defrule term-accusation
  (declare (salience 90))
  (logical (accusation (victim ?v) (color ?c)))
  (not (logodds-term (color ?c) (source accusation) (key =(str-cat ?v))))
  =>
  (assert (logodds-term (color ?c) (source accusation)
                        (key (str-cat ?v)) (amount 1500))))

;; Near a body during the kill window, with no cast-iron alibi: small
;; positive. The `logical` block (which must come first in the rule)
;; ties the term to the case, the near-body fact, AND the absence of a
;; cast-iron alibi -- so a later clearance retracts the term.
(defrule term-near-body
  (declare (salience 90))
  (logical (case (victim ?v) (room ?r) (body-tick ?bt))
           (near-body (color ?c) (room ?r) (tick ?bt))
           (not (cast-iron-alibi (color ?c) (victim ?v))))
  (not (logodds-term (color ?c) (source near-body) (key =(str-cat ?v))))
  =>
  (assert (logodds-term (color ?c) (source near-body)
                        (key (str-cat ?v)) (amount 600))))

;; Off-task (zero visible tasks): very small positive. Not `logical` --
;; it keys on a slot, not a fact. A color that later completes tasks
;; keeps this +150, but term-on-task's -500 then dominates the sum.
(defrule term-off-task
  (declare (salience 90))
  (self-color (color ?self))
  (crewmember (color ?c&~?self) (status alive) (visible-tasks 0))
  (not (logodds-term (color ?c) (source off-task) (key "")))
  =>
  (assert (logodds-term (color ?c) (source off-task) (key "") (amount 150))))

;; On-task (>= the on-task threshold): negative -- visible cooperation.
(defrule term-on-task
  (declare (salience 90))
  (self-color (color ?self))
  (crewmember (color ?c&~?self) (status alive)
              (visible-tasks ?n&:(>= ?n ?*on-task-tasks*)))
  (not (logodds-term (color ?c) (source on-task) (key "")))
  =>
  (assert (logodds-term (color ?c) (source on-task) (key "") (amount -500))))
