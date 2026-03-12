; free variable, bounded forall variable, and bounded exists variable
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun n$ () Nat$)
(assert (! (forall ((x$ Nat$)) (not (= x$ 1))) :named a0))
(assert (! (exists ((x$ Nat$)) (not (= x$ n$))) :named a1))
(check-sat)
