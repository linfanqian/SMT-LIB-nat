; exchanged first example from the other group during midterm reivew
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun f$ (Nat$ Nat$) Int)
(declare-fun g$ (Nat$) Nat$)
(declare-fun leq_nat$ (Nat$ Nat$) Bool)
(assert (! (forall ((x$ Nat$)) (exists ((y$ Nat$)) (leq_nat$ (f$ (g$ x$) y$) (f$ (g$ y$) x$)))) :named a0))
(check-sat)
