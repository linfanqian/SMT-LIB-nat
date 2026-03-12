; exchanged seond example from the other group during midterm reivew
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun f$ (Nat$ Bool) Nat$)
(declare-fun y$ () Nat$)
(declare-fun geq_nat$ (Nat$ Nat$) Bool)
(assert (! (forall ((x$ Nat$) (b$ Bool)) (= (f$ x$ b$) (f$ y$ b$))) :named a0))
(check-sat)
