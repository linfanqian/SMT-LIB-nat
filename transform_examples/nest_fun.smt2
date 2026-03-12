; nested functions
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun f$ (Int) Int)
(declare-fun g$ (Nat$) Int)
(declare-fun h$ (Nat$) Nat$)
(assert (! (exists ((x$ Nat$)) (= (f$ (g$ (h$ x$))) 0)) :named a0))
(check-sat)
