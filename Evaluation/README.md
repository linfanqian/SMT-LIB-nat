- We start from 40 raw selected SMT-LIB benchmarks. From this fixed base set, we generate three benchmark families: Nat$ -> Int, Int -> Nat$, and Nat$ -> Nat$. The generated families in Evaluation/rewritten_problems/ are the final benchmark sets used in evaluation. Running Evaluation/scripts/generate_benchmark_families.sh regenerates these families.

- In scripts, rewrite_selected_to_nat.py is the script that was used to generate Nat$->Int family, inject_nat_step.sh was used to generate Nat$ -> Nat$ family, and inject_composed_bridge_on_natvar_200.sh was used to generate Int-> Nat$ family. 
