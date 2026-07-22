;; social.clp - accusation / defense social-graph reasoning
;; (Enhancement 5).
;;
;; The C side asserts (accuses a b) when `a` votes for / accuses `b`,
;; and (defends a b) when `a` speaks up for `b`. This file emits
;; logodds-term increments that fire RETROACTIVELY: when an ejection
;; reveals an impostor, every color that interacted with that impostor
;; is re-scored. This is the one layer whose evidence correlates ACROSS
;; players -- a single reveal re-reads the whole table.
;;
;; All rules are in the term band (salience 90). ejection / accuses /
;; defends facts are never retracted, so the reveal-driven rules are not
;; `logical`; term-social-pack-cleared is `logical` because its
;; cast-iron-alibi evidence can retract when a kill window narrows.
;; Amounts are milli-nats; tunable. Opt-in -- with no accuses/defends
;; facts fed, nothing here fires.

;; A color that accused the player later revealed as an impostor read
;; the game correctly: a negative (exculpatory) increment.
(defrule term-social-accuse-impostor
  (declare (salience 90))
  (ejection (color ?x) (was-impostor t))
  (accuses (a ?c&~?x) (b ?x))
  (not (logodds-term (color ?c) (source social-accuse-impostor)
                     (key =(str-cat ?x))))
  =>
  (assert (logodds-term (color ?c) (source social-accuse-impostor)
                        (key (str-cat ?x)) (amount -600))))

;; A color that DEFENDED the revealed impostor shielded a killer: a
;; large positive increment -- impostor speech skews toward defense.
(defrule term-social-defend-impostor
  (declare (salience 90))
  (ejection (color ?x) (was-impostor t))
  (defends (a ?c&~?x) (b ?x))
  (not (logodds-term (color ?c) (source social-defend-impostor)
                     (key =(str-cat ?x))))
  =>
  (assert (logodds-term (color ?c) (source social-defend-impostor)
                        (key (str-cat ?x)) (amount 1000))))

;; A color that piled onto the same target the revealed impostor
;; accused -- a coordination signal (impostors coordinate their votes).
;; Modest positive: innocents bandwagon too, so this is weak.
(defrule term-social-co-accuse
  (declare (salience 90))
  (ejection (color ?x) (was-impostor t))
  (accuses (a ?x) (b ?t))
  (accuses (a ?c&~?x) (b ?t))
  (not (logodds-term (color ?c) (source social-co-accuse)
                     (key =(str-cat ?x "-" ?t))))
  =>
  (assert (logodds-term (color ?c) (source social-co-accuse)
                        (key (str-cat ?x "-" ?t)) (amount 300))))

;; A color that accused a player holding a cast-iron alibi -- accusing
;; the provably innocent is misdirection. A small positive increment,
;; keyed on the cleared color so it counts once however many cases
;; cleared them. `logical` on cast-iron-alibi: if the clearing window
;; narrows and the alibi falls, the term retracts with it.
(defrule term-social-pack-cleared
  (declare (salience 90))
  (logical (cast-iron-alibi (color ?cleared) (victim ?v)))
  (accuses (a ?c&~?cleared) (b ?cleared))
  (not (logodds-term (color ?c) (source social-pack-cleared)
                     (key =(str-cat ?cleared))))
  =>
  (assert (logodds-term (color ?c) (source social-pack-cleared)
                        (key (str-cat ?cleared)) (amount 500))))
