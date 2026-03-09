# Introduction
This project explores the ways to lift an SMT-LIB problem involving natural numbers to the version of Int. \
We assume every input problem mentioned below is a natural-number SMT-LIB problem.

# Input Problem Format
The input problem should be in a .smt2 file.
You should declare an uninterpreted sort Nat$ at the top: `(declare-sort Nat$ 0)`. Every natural number should have the sort `Nat$`. \
The function declarations for arithmetic operations should follow a spacial pattern as listed below:
| Starts with (In Input Porblem) | Map to (In Transformed Problem) |
| --- | --- |
| `div_` | div |
| `mod_` | mod |
| `pow2_` | int.pow2 |
| `ispow2_` | int.ispow2 |
| `log2_` | int.log2 |
| `add_` | + |
| `sub_` | - (binary) |
| `neg_` | - (unary) |
| `mult_` | * |
| `abs_` | abs |
| `pow_` | ^ |
| `lt_` | < |
| `leq_` | <= |
| `gt_` | > |
| `geq_` | >= |

### Examples of Declared Functions Consuming and/or Returning Nat$
- (declare-fun f$ (Nat$) Int)
- (declare-fun g$ (Int) Nat$)
- (declare-fun h$ (Nat$) Nat$)
- (declare-fun add_anySuffixYouWantToAdd$ (Nat$ Nat$) Nat$)
- (declare-fun mult_anySuffixYouWantToAdd$ (Nat$ Int) Nat$)

# How to Use CVC5
## Compilation
Plese follow https://github.com/cvc5/cvc5/blob/main/INSTALL.rst to compile cvc5-1.3.2 in this repo. \
To switch between total definition and partial definition, modify `USE_TOTAL_DEFINITION` in cvc5-1.3.2/src/preprocessing/passes/nat_to_int.cpp. \
First-time compilation may take up to 30-40 minutes depending on your system. Later compilation after code modifications will take less than 1 min.

## Transform Input Problem
If you only want to get the transformed problem instead of getting a sat/unsat/unknown result, run `cvc5-1.3.2/nat_to_int.sh <intput_file>`

## Run CVC5 directly on the Input Problem
Run `cvc5-1.3.2/run_cvc5.sh <input_file>`. This Shell script assumes the build directory is cvc5-1.3.2/build.