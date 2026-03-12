; numeral handling for Nat$
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun ge_nat$ (Nat$ Nat$) Bool)
(declare-fun add_nat$ (Nat$ Nat$) Nat$)
(assert (! (ge_nat$ (add_nat$ 5 10) 20) :named a0))
(check-sat)
