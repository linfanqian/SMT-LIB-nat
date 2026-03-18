/* The natural number preprocessing pass.
 *
 * Lift all sort Nat$ in variables and functions to Int,
 * adding non-negativity constraints:
 *
 *   Free variables:
 *     For each x :: Nat$, introduce x' :: Int and assert x' >= 0 (top-level).
 *
 *   Bound variables (quantifiers):
 *     forall [v :: Nat$] body  -->  forall [v' :: Int] (v' >= 0) -> body'
 *     exists [v :: Nat$] body  -->  exists [v' :: Int] (v' >= 0) /\ body'
 *
 *   Functions — two methods selectable via USE_TOTAL_DEFINITION in nat_to_int.cpp:
 *
 *     Total definition (USE_TOTAL_DEFINITION = true, default):
 *       For each f :: x1,...,xm, a1,...,an -> Nat$ where xi are Nat$, add a top-level axiom
 *         forall x1',...,xm',a1,...,an. (xi' >= 0) -> f'(x1',...,xm') >= 0
 *
 *     Partial definition (USE_TOTAL_DEFINITION = false):
 *       For every atomic formula, recursively find functions f(args) -> Nat$.
 *       Add f'(args') >= 0 at the level of atomic formula.
 */

#include "cvc5_private.h"

#ifndef CVC5__PREPROCESSING__PASSES__NAT_TO_INT_H
#define CVC5__PREPROCESSING__PASSES__NAT_TO_INT_H

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "context/cdhashmap.h"
#include "context/cdhashset.h"
#include "expr/node.h"
#include "expr/node_manager.h"
#include "preprocessing/preprocessing_pass.h"

namespace cvc5::internal {
namespace preprocessing {
namespace passes {

class NatToInt : public PreprocessingPass
{
 public:
  NatToInt(PreprocessingPassContext* preprocContext);

 protected:
  PreprocessingPassResult applyInternal(
      AssertionPipeline* assertionsToPreprocess) override;

 private:
  using NodeMap = context::CDHashMap<Node, Node>;
  using NodeSet = context::CDHashSet<Node>;

  NodeManager* d_nm;

  /** original Nat$ variable (free or bound) -> its Int analogue */
  NodeMap d_varNatToInt;
  /** original non-arithmetic Nat$-related UF -> its Int-signature analogue */
  NodeMap d_funcNatToInt;
  /** original Nat$-related function -> built-in arithmetic Kind */
  std::unordered_map<Node, Kind> d_funcArithKind;
  /** lifted UF nodes whose original return type was Nat$ */
  NodeSet d_liftedNatRetFuncs;
  /** lifted arithmetic nodes that originated from a Nat$-returning function */
  NodeSet d_arithNatRetApps;
  /** lifted Int bound variables that were originally Nat$ bound variables */
  NodeSet d_liftedNatBVarSet;
  /** memoisation: original node -> structurally-lifted node */
  NodeMap d_lifted;

  /** Return the arithmetic Kind encoded by the function name prefix,
   *  or Kind::UNDEFINED_KIND if the name does not match any prefix. */
  static Kind prefixToArithKind(const std::string& name);

  // -----------------------------------------------------------------------
  // Type helpers
  // -----------------------------------------------------------------------
  bool isNat(const TypeNode& tn) const;
  bool hasNatInSignature(const TypeNode& tn) const;
  TypeNode createIntAnalogue(const TypeNode& tn);

  // -----------------------------------------------------------------------
  // Symbol collection
  // -----------------------------------------------------------------------
  /** Collect Nat$-related symbols from root into four disjoint sets:
   *    natFreeVars   — free variables of sort Nat$
   *    natUFFuncs    — non-arithmetic UFs with Nat$ in their signature
   *    natArithFuncs — arithmetic-prefixed functions with Nat$ in signature
   *    natQuantifiers— FORALL/EXISTS nodes that have at least one Nat$
   *                    bound variable */
  void collectNatSymbols(TNode root,
                         std::unordered_set<Node>& natFreeVars,
                         std::unordered_set<Node>& natUFFuncs,
                         std::unordered_set<Node>& natArithFuncs,
                         std::unordered_set<Node>& natQuantifiers,
                         std::unordered_set<Node>& visited);

  // -----------------------------------------------------------------------
  // Lifting
  // -----------------------------------------------------------------------
  /** Pure structural lift: replaces Nat$ vars/funcs with their Int analogues
   *  (looked up in d_varNatToInt / d_funcNatToInt / d_funcArithKind).
   *  All symbols must be pre-registered before calling this.
   *  Results are cached in d_lifted. */
  Node liftNodeInternal(TNode n);

  /** Add a top-level non-negativity assertion for a free Nat$ variable.
   *  Requires natVar to already be registered in d_varNatToInt. */
  void liftFreeVar(TNode natVar, AssertionPipeline* ap);

  /** Given a single already-lifted FORALL/EXISTS node whose bound-variable
   *  list contains variables in d_liftedNatBVarSet, return a new node with
   *  the body wrapped by the guard:
   *    FORALL: (v' >= 0 /\ ...) -> body
   *    EXISTS: (v' >= 0 /\ ...) /\ body
   *  Does not recurse — all quantifiers are pre-collected by collectNatSymbols
   *  and processed individually by the caller. */
  Node liftBoundVar(TNode liftedQ);

  // -----------------------------------------------------------------------
  // Total definition axiom
  // -----------------------------------------------------------------------
  /** Build and append the global non-negativity forall axiom for natFunc.
   *  Handles both arithmetic-prefixed functions (via d_funcArithKind) and
   *  UFs (via d_funcNatToInt).  Does nothing if natFunc does not return Nat$. */
  void addTotalAxiom(TNode natFunc, AssertionPipeline* ap);

  // -----------------------------------------------------------------------
  // Partial definition constraint injection
  // -----------------------------------------------------------------------
  /** Step 1: identify each atomic formula in an already-lifted formula.
   *  Step 2: collect Nat$-returning applications in its subterms.
   *  Step 3: conjunct f'(args) >= 0 for each such application at the
   *  level of that atomic formula.
   *  Logical connectives (AND, OR, IMPLIES) and quantifiers (FORALL, EXISTS)
   *  are traversed transparently. */
  Node injectPartialConstraints(TNode liftedFormula);

  /** Walk an already-lifted subterm and append to apps every sub-expression
   *  that is an application of a Nat$-returning lifted function
   *  (either a UF in d_liftedNatRetFuncs or an arithmetic node in
   *  d_arithNatRetApps).  Recurses into all children including quantifiers. */
  void collectNatRetApps(TNode liftedExpr, std::vector<Node>& apps);
};

}  // namespace passes
}  // namespace preprocessing
}  // namespace cvc5::internal

#endif /* CVC5__PREPROCESSING__PASSES__NAT_TO_INT_H */
