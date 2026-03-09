/* The natural number preprocessing pass.
 *
 * Lift all variables, bounded variables, and functions of sort Nat$ to Int,
 * adding non-negativity constraints:
 *
 *   Free variables:
 *     For each x :: Nat$, introduce x' :: Int and assert x' >= 0 (top-level).
 *
 *   Bounded variables (quantifiers):
 *     FORALL [v :: Nat$] body  -->  FORALL [v' :: Int] (v' >= 0) -> body'
 *     EXISTS [v :: Nat$] body  -->  EXISTS [v' :: Int] (v' >= 0) /\ body'
 *
 *   Functions — two methods selectable via USE_TOTAL_DEFINITION in nat_to_int.cpp:
 *
 *     Total definition (USE_TOTAL_DEFINITION = true, default):
 *       For each f :: T1 x...x Tn -> Nat$, add a top-level axiom
 *         forall x1,...,xn. (xi >= 0 for every Nat$ arg xi) => f'(x1,...,xn) >= 0
 *
 *     Partial definition (USE_TOTAL_DEFINITION = false):
 *       For every function application f'(args) where f originally consumed
 *       and/or returned Nat$ (i.e. f is in the Nat$-related set), add >= 0
 *       constraints for every Nat$-typed sub-expression inside f'(args) that
 *       is not a free variable (free variables already have their own top-level
 *       >= 0 assertion), plus >= 0 for f'(args) itself when f returned Nat$.
 *       All such constraints are placed at the *same quantifier scope* as the
 *       function application:
 *         - top-level occurrence -> separate top-level assertions
 *         - occurrence in a quantifier body -> ANDed into that body
 *       Stops at FORALL/EXISTS boundaries: inner constraints are already
 *       folded in by liftQuantifier.
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

  /** original Nat$ variable/bound-var -> its Int analogue */
  NodeMap d_varNatToInt;
  /** original Nat$-related function -> its Int-signature analogue (UF) */
  NodeMap d_funcNatToInt;
  /** original Nat$-related function -> built-in arithmetic Kind
   *  (populated when the function name starts with a recognised prefix) */
  std::unordered_map<Node, Kind> d_funcArithKind;
  /** lifted function nodes whose original return type was Nat$ */
  NodeSet d_liftedNatRetFuncs;
  /** lifted arithmetic nodes (e.g. (+ a b)) that originated from a
   *  Nat$-returning arithmetic-prefixed function; used by partial def */
  NodeSet d_arithNatRetApps;
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
  /** Collect free Nat$ variables (into natVars) and Nat$-related function
   *  symbols (into natFuncs) that appear in the tree rooted at root. */
  void collectNatSymbols(TNode root,
                         std::unordered_set<Node>& natVars,
                         std::unordered_set<Node>& natFuncs,
                         std::unordered_set<Node>& visited);

  // -----------------------------------------------------------------------
  // Lifting
  // -----------------------------------------------------------------------
  /** Pure structural lift: replaces Nat$ vars/funcs with Int analogues.
   *  Handles FORALL/EXISTS via liftQuantifier.
   *  Results are cached in d_lifted. */
  Node liftNodeInternal(TNode n);

  /** Lift a FORALL or EXISTS node:
   *  1. Create fresh Int bound-vars for Nat$ bound-vars and register in
   *     d_varNatToInt so that body processing picks them up.
   *  2. Lift the body via liftNodeInternal.
   *  3. (Partial def) collect same-scope constraints from the lifted body
   *     and AND them into the body before applying the nat-var guard.
   *  4. Wrap with (v' >= 0) -> ... for FORALL, (v' >= 0) /\ ... for EXISTS.
   *  Returns the lifted quantifier; no constraints escape upward. */
  Node liftQuantifier(TNode n);

  // -----------------------------------------------------------------------
  // Partial definition constraint collection
  // -----------------------------------------------------------------------
  /** Walk an already-lifted expression and append to out a ">= 0" node for
   *  every sub-expression that is an application of a Nat$-returning lifted
   *  function.  This captures both the return-value constraint AND the
   *  constraint for Nat$-typed arguments/sub-arguments:
   *    - Nat$-returning applications at any depth yield their own >= 0.
   *    - Non-Nat$-returning applications are transparent (we recurse into
   *      their arguments), so any Nat$-typed argument is still found.
   *  Stops at FORALL/EXISTS boundaries; inner constraints are already folded
   *  into the body by liftQuantifier and must not be re-emitted at an outer
   *  scope. */
  void collectPartialConstraints(TNode liftedExpr,
                                 std::vector<Node>& out);
};

}  // namespace passes
}  // namespace preprocessing
}  // namespace cvc5::internal

#endif /* CVC5__PREPROCESSING__PASSES__NAT_TO_INT_H */
