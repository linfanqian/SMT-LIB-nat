/* The natural number preprocessing pass.
 *
 * See nat_to_int.h for a full description.
 *
 * ---- method switch ----
 * Set USE_TOTAL_DEFINITION to true  -> total definition for functions
 *                             false -> partial definition for functions
 * This file has Claude Code generated code
 */

#include "preprocessing/passes/nat_to_int.h"

#include "preprocessing/assertion_pipeline.h"
#include "preprocessing/preprocessing_pass_context.h"
#include "util/rational.h"

using namespace cvc5::internal::kind;

namespace cvc5::internal {
namespace preprocessing {
namespace passes {

// ---------------------------------------------------------------------------
// Switch between the two function-lifting methods.
// ---------------------------------------------------------------------------
static constexpr bool USE_TOTAL_DEFINITION = true;

// ===========================================================================
// Prefix → arithmetic Kind table
// ===========================================================================

Kind NatToInt::prefixToArithKind(const std::string& name)
{
  static const struct { const char* prefix; Kind k; } table[] = {
    { "div_",    Kind::INTS_DIVISION },
    { "mod_",    Kind::INTS_MODULUS  },
    { "pow2_",   Kind::POW2          },
    { "ispow2_", Kind::INTS_ISPOW2   },
    { "log2_",   Kind::INTS_LOG2     },
    { "add_",    Kind::ADD            },
    { "sub_",    Kind::SUB            },
    { "neg_",    Kind::NEG            },
    { "mult_",   Kind::MULT           },
    { "abs_",    Kind::ABS            },
    { "pow_",    Kind::POW            },
    { "lt_",     Kind::LT             },
    { "leq_",    Kind::LEQ            },
    { "gt_",     Kind::GT             },
    { "geq_",    Kind::GEQ            },
  };
  for (const auto& entry : table)
  {
    if (name.rfind(entry.prefix, 0) == 0) return entry.k;
  }
  return Kind::UNDEFINED_KIND;
}

// ===========================================================================
// Constructor
// ===========================================================================

NatToInt::NatToInt(PreprocessingPassContext* preprocContext)
    : PreprocessingPass(preprocContext, "nat-to-int"),
      d_nm(nodeManager()),
      d_varNatToInt(userContext()),
      d_funcNatToInt(userContext()),
      d_liftedNatRetFuncs(userContext()),
      d_arithNatRetApps(userContext()),
      d_liftedNatBVarSet(userContext()),
      d_lifted(userContext())
{
}

// Forward declaration — defined after liftBoundVar below.
static Node applyBoundVarSubst(NodeManager* nm,
                                TNode n,
                                const std::unordered_map<Node, Node>& subst,
                                std::unordered_map<Node, Node>& cache);

// ===========================================================================
// applyInternal
// ===========================================================================

PreprocessingPassResult NatToInt::applyInternal(
    AssertionPipeline* assertionsToPreprocess)
{
  // ------------------------------------------------------------------
  // Pre-work: collect Nat$-related symbols from all assertions.
  // ------------------------------------------------------------------
  std::unordered_set<Node> natFreeVars;
  std::unordered_set<Node> natUFFuncs;
  std::unordered_set<Node> natArithFuncs;
  std::unordered_set<Node> natQuantifiers;
  std::unordered_set<Node> visited;

  for (size_t i = 0, n = assertionsToPreprocess->size(); i < n; ++i)
    collectNatSymbols((*assertionsToPreprocess)[i],
                      natFreeVars, natUFFuncs, natArithFuncs,
                      natQuantifiers, visited);

  // ------------------------------------------------------------------
  // Step 1: Rewrite Nat$ to Int (shared).
  //   1a. Register each free Nat$ variable as a fresh Int variable.
  //   1b. For each quantifier in natQuantifiers, register each of its
  //       Nat$ bound variables as a fresh Int bound variable and record
  //       it in d_liftedNatBVarSet for step 2b.
  //   1c. Register each non-arithmetic Nat$-related UF as its Int analogue.
  //   1d. Register each arithmetic Nat$-related function in d_funcArithKind.
  //   1e. Structurally lift every assertion (all symbols pre-registered,
  //       so liftNodeInternal performs pure substitution — no side actions).
  // ------------------------------------------------------------------

  // 1a: free variable registration
  for (const Node& natVar : natFreeVars)
  {
    Node intVar = NodeManager::mkRawSymbol(
        "lift_" + natVar.getName(), d_nm->integerType());
    d_varNatToInt.insert(natVar, intVar);
  }

  // 1b: bound variable registration (extracted from quantifier nodes)
  for (const Node& q : natQuantifiers)
  {
    for (TNode bv : q[0])
    {
      if (isNat(bv.getType()) && d_varNatToInt.find(bv) == d_varNatToInt.end())
      {
        Node intBv = NodeManager::mkBoundVar(
            "lift_" + bv.getName(), d_nm->integerType());
        d_varNatToInt.insert(bv, intBv);
        d_liftedNatBVarSet.insert(intBv);
      }
    }
  }

  // 1c: UF registration
  for (const Node& natFunc : natUFFuncs)
  {
    TypeNode origFuncType = natFunc.getType();
    size_t   arity        = origFuncType.getNumChildren() - 1;
    bool     retWasNat    = isNat(origFuncType[arity]);

    TypeNode intFuncType = createIntAnalogue(origFuncType);
    Node intFunc = NodeManager::mkRawSymbol(
        "lift_" + natFunc.getName(), intFuncType);
    d_funcNatToInt.insert(natFunc, intFunc);
    if (retWasNat)
      d_liftedNatRetFuncs.insert(intFunc);
  }

  // 1d: arithmetic function registration
  for (const Node& natFunc : natArithFuncs)
    d_funcArithKind[natFunc] = prefixToArithKind(natFunc.getName());

  // 1e: structural lift
  size_t liftedCount = assertionsToPreprocess->size();
  for (size_t i = 0; i < liftedCount; ++i)
  {
    Node orig   = (*assertionsToPreprocess)[i];
    Node lifted = liftNodeInternal(orig);
    if (lifted != orig)
      assertionsToPreprocess->replace(i, lifted);
  }

  // ------------------------------------------------------------------
  // Step 2: Add non-negativity constraints for variables (shared).
  //   2a. Free variables: append lift_v >= 0 as a top-level assertion.
  //   2b. Bound variables: for each quantifier in natQuantifiers, wrap
  //       its lifted body with the guard.  All quantifiers were
  //       pre-collected, so liftBoundVar operates on one node at a time
  //       without recursion.  The results are substituted back into the
  //       lifted assertions via applyBoundVarSubst.
  // ------------------------------------------------------------------

  // 2a: free variables
  for (const Node& natVar : natFreeVars)
    liftFreeVar(natVar, assertionsToPreprocess);

  // 2b: bound variables — build substitution map, then apply
  std::unordered_map<Node, Node> qSubst;
  for (const Node& q : natQuantifiers)
  {
    NodeMap::iterator it = d_lifted.find(q);
    if (it == d_lifted.end()) continue;
    Node lifted_q  = (*it).second;
    Node wrapped_q = liftBoundVar(lifted_q);
    if (wrapped_q != lifted_q)
      qSubst[lifted_q] = wrapped_q;
  }
  if (!qSubst.empty())
  {
    std::unordered_map<Node, Node> substCache;
    for (size_t i = 0; i < liftedCount; ++i)
    {
      Node orig   = (*assertionsToPreprocess)[i];
      Node result = applyBoundVarSubst(d_nm, orig, qSubst, substCache);
      if (result != orig)
        assertionsToPreprocess->replace(i, result);
    }
  }

  // ------------------------------------------------------------------
  // Step 3: Add non-negativity constraints for functions (branched).
  //   Total:   one global forall axiom per Nat$-returning function.
  //   Partial: inject f'(args) >= 0 as a conjunct at each atomic
  //            formula containing a Nat$-returning application.
  // ------------------------------------------------------------------
  if (USE_TOTAL_DEFINITION)
  {
    for (const Node& f : natUFFuncs)   addTotalAxiom(f, assertionsToPreprocess);
    for (const Node& f : natArithFuncs) addTotalAxiom(f, assertionsToPreprocess);
  }
  else
  {
    for (size_t i = 0; i < liftedCount; ++i)
    {
      Node orig     = (*assertionsToPreprocess)[i];
      Node injected = injectPartialConstraints(orig);
      if (injected != orig)
        assertionsToPreprocess->replace(i, injected);
    }
  }

  return PreprocessingPassResult::NO_CONFLICT;
}

// ===========================================================================
// Type helpers
// ===========================================================================

bool NatToInt::isNat(const TypeNode& tn) const
{
  return tn.isUninterpretedSort() && tn.getName() == "Nat$";
}

bool NatToInt::hasNatInSignature(const TypeNode& tn) const
{
  if (!tn.isFunction()) return false;
  for (size_t i = 0, n = tn.getNumChildren(); i < n; ++i)
  {
    if (isNat(tn[i])) return true;
  }
  return false;
}

TypeNode NatToInt::createIntAnalogue(const TypeNode& tn)
{
  if (isNat(tn)) return d_nm->integerType();
  if (tn.isFunction())
  {
    size_t arity = tn.getNumChildren() - 1;
    std::vector<TypeNode> argTypes;
    argTypes.reserve(arity);
    for (size_t i = 0; i < arity; ++i)
      argTypes.push_back(createIntAnalogue(tn[i]));
    return d_nm->mkFunctionType(argTypes, createIntAnalogue(tn[arity]));
  }
  return tn;
}

// ===========================================================================
// Symbol collection
// ===========================================================================

void NatToInt::collectNatSymbols(TNode root,
                                 std::unordered_set<Node>& natFreeVars,
                                 std::unordered_set<Node>& natUFFuncs,
                                 std::unordered_set<Node>& natArithFuncs,
                                 std::unordered_set<Node>& natQuantifiers,
                                 std::unordered_set<Node>& visited)
{
  std::vector<TNode> toVisit;
  toVisit.push_back(root);

  while (!toVisit.empty())
  {
    TNode cur = toVisit.back();
    toVisit.pop_back();

    if (visited.count(cur)) continue;
    visited.insert(cur);

    // For APPLY_UF the function symbol is an operator, not a regular child.
    if (cur.getMetaKind() == metakind::PARAMETERIZED)
    {
      Node op = cur.getOperator();
      if (!visited.count(op))
      {
        visited.insert(op);
        TypeNode opType = op.getType();
        if (opType.isFunction() && hasNatInSignature(opType))
        {
          if (prefixToArithKind(op.getName()) != Kind::UNDEFINED_KIND)
            natArithFuncs.insert(op);
          else
            natUFFuncs.insert(op);
        }
      }
    }

    // Collect quantifiers that have at least one Nat$ bound variable.
    if (cur.getKind() == Kind::FORALL || cur.getKind() == Kind::EXISTS)
    {
      for (TNode bv : cur[0])
      {
        if (isNat(bv.getType()))
        {
          natQuantifiers.insert(cur);
          break;
        }
      }
    }

    if (cur.getNumChildren() == 0)
    {
      TypeNode tn = cur.getType();
      if (isNat(tn)
          && cur.getKind() != Kind::BOUND_VARIABLE
          && cur.getKind() != Kind::CONST_INTEGER)
      {
        natFreeVars.insert(cur);
      }
    }
    else
    {
      for (TNode child : cur)
        toVisit.push_back(child);
    }
  }
}

// ===========================================================================
// Structural lift
// ===========================================================================

Node NatToInt::liftNodeInternal(TNode n)
{
  // Memoisation
  {
    NodeMap::iterator it = d_lifted.find(n);
    if (it != d_lifted.end()) return (*it).second;
  }

  Node result;
  Kind k = n.getKind();

  // Leaf: integer literal re-emitted as Int constant; other leaves looked up.
  if (n.getNumChildren() == 0)
  {
    if (k == Kind::CONST_INTEGER)
    {
      result = d_nm->mkConstInt(n.getConst<Rational>());
    }
    else
    {
      NodeMap::iterator vit = d_varNatToInt.find(n);
      result = (vit != d_varNatToInt.end()) ? (*vit).second : Node(n);
    }
    d_lifted.insert(n, result);
    return result;
  }

  // Non-leaf PARAMETERIZED: check for arithmetic rewrite first.
  if (n.getMetaKind() == metakind::PARAMETERIZED)
  {
    Node op = n.getOperator();

    auto arithIt = d_funcArithKind.find(op);
    if (arithIt != d_funcArithKind.end())
    {
      std::vector<Node> arithArgs;
      for (TNode child : n)
        arithArgs.push_back(liftNodeInternal(child));
      result = d_nm->mkNode(arithIt->second, arithArgs);
      TypeNode opType = op.getType();
      if (isNat(opType[opType.getNumChildren() - 1]))
        d_arithNatRetApps.insert(result);
      d_lifted.insert(n, result);
      return result;
    }

    // UF: substitute operator if registered, then recurse into arguments.
    NodeMap::iterator fop = d_funcNatToInt.find(op);
    Node newOp = (fop != d_funcNatToInt.end()) ? (*fop).second : op;
    bool changed = (newOp != op);
    std::vector<Node> newChildren = {newOp};
    for (TNode child : n)
    {
      Node nc = liftNodeInternal(child);
      newChildren.push_back(nc);
      changed = changed || (nc != child);
    }
    result = changed ? d_nm->mkNode(k, newChildren) : Node(n);
    d_lifted.insert(n, result);
    return result;
  }

  // Non-leaf non-PARAMETERIZED (includes FORALL, EXISTS, connectives, etc.):
  // recurse into all children.  Nat$ bound variables in BOUND_VAR_LISTs have
  // been pre-registered in d_varNatToInt and are substituted as leaves.
  bool changed = false;
  std::vector<Node> newChildren;
  for (TNode child : n)
  {
    Node nc = liftNodeInternal(child);
    newChildren.push_back(nc);
    changed = changed || (nc != child);
  }
  result = changed ? d_nm->mkNode(k, newChildren) : Node(n);
  d_lifted.insert(n, result);
  return result;
}

// ===========================================================================
// Free variable constraint
// ===========================================================================

void NatToInt::liftFreeVar(TNode natVar, AssertionPipeline* ap)
{
  NodeMap::iterator it = d_varNatToInt.find(natVar);
  Assert(it != d_varNatToInt.end());
  Node zero = d_nm->mkConstInt(Rational(0));
  ap->push_back(d_nm->mkNode(Kind::GEQ, (*it).second, zero));
}

// ===========================================================================
// Bound variable constraint
// ===========================================================================

Node NatToInt::liftBoundVar(TNode liftedQ)
{
  Kind k = liftedQ.getKind();
  Assert(k == Kind::FORALL || k == Kind::EXISTS);

  Node zero = d_nm->mkConstInt(Rational(0));
  std::vector<Node> guards;
  for (TNode bv : liftedQ[0])
  {
    if (d_liftedNatBVarSet.find(Node(bv)) != d_liftedNatBVarSet.end())
      guards.push_back(d_nm->mkNode(Kind::GEQ, Node(bv), zero));
  }

  if (guards.empty()) return Node(liftedQ);

  Node guard = guards.size() == 1
                   ? guards[0]
                   : d_nm->mkNode(Kind::AND, guards);
  Node guardedBody = (k == Kind::FORALL)
                         ? d_nm->mkNode(Kind::IMPLIES, guard, liftedQ[1])
                         : d_nm->mkNode(Kind::AND, guard, liftedQ[1]);
  return liftedQ.getNumChildren() > 2
             ? d_nm->mkNode(k, liftedQ[0], guardedBody, liftedQ[2])
             : d_nm->mkNode(k, liftedQ[0], guardedBody);
}

// ===========================================================================
// Quantifier substitution helper
// ===========================================================================

static Node applyBoundVarSubst(NodeManager* nm,
                                TNode n,
                                const std::unordered_map<Node, Node>& subst,
                                std::unordered_map<Node, Node>& cache)
{
  auto cit = cache.find(Node(n));
  if (cit != cache.end()) return cit->second;

  auto sit = subst.find(Node(n));
  if (sit != subst.end())
  {
    // Recurse into the substituted result so nested quantifiers are handled.
    Node result = applyBoundVarSubst(nm, sit->second, subst, cache);
    cache[Node(n)] = result;
    return result;
  }

  if (n.getNumChildren() == 0)
  {
    cache[Node(n)] = Node(n);
    return Node(n);
  }

  bool changed = false;
  std::vector<Node> newChildren;
  if (n.getMetaKind() == metakind::PARAMETERIZED)
    newChildren.push_back(n.getOperator());
  for (TNode child : n)
  {
    Node nc = applyBoundVarSubst(nm, child, subst, cache);
    newChildren.push_back(nc);
    changed = changed || (nc != child);
  }
  Node result = changed ? nm->mkNode(n.getKind(), newChildren) : Node(n);
  cache[Node(n)] = result;
  return result;
}

// ===========================================================================
// Total definition axiom
// ===========================================================================

void NatToInt::addTotalAxiom(TNode natFunc, AssertionPipeline* ap)
{
  TypeNode origFuncType = natFunc.getType();
  size_t   arity        = origFuncType.getNumChildren() - 1;
  bool     retWasNat    = isNat(origFuncType[arity]);
  if (!retWasNat) return;

  Node zero      = d_nm->mkConstInt(Rational(0));
  Kind arithKind = prefixToArithKind(natFunc.getName());

  if (arithKind != Kind::UNDEFINED_KIND)
  {
    // Arithmetic op returning Nat$: forall over Int bound vars.
    if (arity == 0) return;
    TypeNode intFuncType = createIntAnalogue(origFuncType);
    std::vector<Node> bvars;
    for (size_t i = 0; i < arity; ++i)
      bvars.push_back(NodeManager::mkBoundVar(
          "x" + std::to_string(i), intFuncType[i]));

    std::vector<Node> premises;
    for (size_t i = 0; i < arity; ++i)
      if (isNat(origFuncType[i]))
        premises.push_back(d_nm->mkNode(Kind::GEQ, bvars[i], zero));

    Node app        = d_nm->mkNode(arithKind, bvars);
    Node conclusion = d_nm->mkNode(Kind::GEQ, app, zero);
    Node body = premises.empty()       ? conclusion
                : premises.size() == 1 ? d_nm->mkNode(Kind::IMPLIES,
                                                       premises[0], conclusion)
                                       : d_nm->mkNode(Kind::IMPLIES,
                                                      d_nm->mkNode(Kind::AND, premises),
                                                      conclusion);
    ap->push_back(d_nm->mkNode(Kind::FORALL,
                               d_nm->mkNode(Kind::BOUND_VAR_LIST, bvars),
                               body));
    return;
  }

  // UF returning Nat$: forall axiom using the registered Int symbol.
  NodeMap::iterator fit = d_funcNatToInt.find(natFunc);
  Assert(fit != d_funcNatToInt.end());
  Node     intFunc     = (*fit).second;
  TypeNode intFuncType = intFunc.getType();

  std::vector<Node> bvars;
  for (size_t i = 0; i < arity; ++i)
    bvars.push_back(NodeManager::mkBoundVar(
        "x" + std::to_string(i), intFuncType[i]));

  std::vector<Node> premises;
  for (size_t i = 0; i < arity; ++i)
    if (isNat(origFuncType[i]))
      premises.push_back(d_nm->mkNode(Kind::GEQ, bvars[i], zero));

  std::vector<Node> appArgs = {intFunc};
  appArgs.insert(appArgs.end(), bvars.begin(), bvars.end());
  Node app = (arity == 0) ? intFunc
                          : d_nm->mkNode(Kind::APPLY_UF, appArgs);
  Node conclusion = d_nm->mkNode(Kind::GEQ, app, zero);
  Node body = premises.empty()       ? conclusion
              : premises.size() == 1 ? d_nm->mkNode(Kind::IMPLIES,
                                                     premises[0], conclusion)
                                     : d_nm->mkNode(Kind::IMPLIES,
                                                    d_nm->mkNode(Kind::AND, premises),
                                                    conclusion);
  Node axiom = (arity == 0)
                   ? body
                   : d_nm->mkNode(Kind::FORALL,
                                  d_nm->mkNode(Kind::BOUND_VAR_LIST, bvars),
                                  body);
  ap->push_back(axiom);
}

// ===========================================================================
// Partial definition — constraint injection
// ===========================================================================

Node NatToInt::injectPartialConstraints(TNode n)
{
  Kind k = n.getKind();

  // Quantifiers: recurse into the body only (n[1]); bound-var list and
  // optional pattern list are left untouched.
  if (k == Kind::FORALL || k == Kind::EXISTS)
  {
    Node newBody = injectPartialConstraints(n[1]);
    if (newBody == n[1]) return Node(n);
    return n.getNumChildren() > 2
               ? d_nm->mkNode(k, n[0], newBody, n[2])
               : d_nm->mkNode(k, n[0], newBody);
  }

  // Logical connectives: recurse so constraints land at atomic formulas.
  if (k == Kind::AND || k == Kind::OR || k == Kind::IMPLIES)
  {
    bool changed = false;
    std::vector<Node> newChildren;
    for (TNode child : n)
    {
      Node nc = injectPartialConstraints(child);
      newChildren.push_back(nc);
      changed = changed || (nc != child);
    }
    return changed ? d_nm->mkNode(k, newChildren) : Node(n);
  }

  // Atomic formula (step 1): collect Nat$-returning applications (step 2)
  // and conjunct their >= 0 constraints (step 3).
  std::vector<Node> apps;
  collectNatRetApps(n, apps);
  if (apps.empty()) return Node(n);

  Node zero = d_nm->mkConstInt(Rational(0));
  std::vector<Node> conjuncts;
  for (const Node& app : apps)
    conjuncts.push_back(d_nm->mkNode(Kind::GEQ, app, zero));
  conjuncts.push_back(Node(n));
  return d_nm->mkNode(Kind::AND, conjuncts);
}

// ===========================================================================
// Partial definition — Nat$-returning application collection
// ===========================================================================

void NatToInt::collectNatRetApps(TNode liftedExpr, std::vector<Node>& apps)
{
  // UF application whose operator originally returned Nat$.
  if (liftedExpr.getMetaKind() == metakind::PARAMETERIZED)
  {
    Node op = liftedExpr.getOperator();
    if (d_liftedNatRetFuncs.find(op) != d_liftedNatRetFuncs.end())
      apps.push_back(Node(liftedExpr));
  }

  // Arithmetic node that originated from a Nat$-returning arithmetic function.
  if (d_arithNatRetApps.find(Node(liftedExpr)) != d_arithNatRetApps.end())
    apps.push_back(Node(liftedExpr));

  // Recurse into all subterms including quantifier bodies.
  for (TNode child : liftedExpr)
    collectNatRetApps(child, apps);
}

}  // namespace passes
}  // namespace preprocessing
}  // namespace cvc5::internal
