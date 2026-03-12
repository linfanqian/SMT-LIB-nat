; exchanged bounded variable example during midterm reivew
(set-logic ALL)
(declare-sort Nat$ 0)
(declare-fun f$ (Nat$) Nat$)
(declare-fun gt_nat$ (Nat$ Nat$) Bool)
(assert (! (forall ((n$ Nat$)) (=> (gt_nat$ n$ 3) (gt_nat$ (f$ n$) n$))) :named a0))
(check-sat)
