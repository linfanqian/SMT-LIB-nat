/* The natural number preprocessing pass.
 *
 * See nat_to_int.h for a full description.
 *
 * ---- method switch ----
 * Set USE_TOTAL_DEFINITION to true  -> total  definition for functions
 *                              false -> partial definition for functions
 */

#include "preprocessing/passes/nat_to_int.h"

#include <iostream>

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
      d_lifted(userContext())
{
}

// ===========================================================================
// applyInternal
// ===========================================================================

PreprocessingPassResult NatToInt::applyInternal(
    AssertionPipeline* assertionsToPreprocess)
{
  // ------------------------------------------------------------------
  // Phase 1: collect Nat$-related symbols from all assertions
  // ------------------------------------------------------------------
  std::unordered_set<Node> natVars;
  std::unordered_set<Node> natFuncs;
  std::unordered_set<Node> visited;

  for (size_t i = 0, n = assertionsToPreprocess->size(); i < n; ++i)
  {
    collectNatSymbols(
        (*assertionsToPreprocess)[i], natVars, natFuncs, visited);
  }

  std::cerr << "[NatToInt] applyInternal: "
            << assertionsToPreprocess->size() << " assertions, "
            << natVars.size() << " natVars, "
            << natFuncs.size() << " natFuncs\n";
  for (const Node& v : natVars)
    std::cerr << "  natVar: " << v << " :: " << v.getType() << "\n";
  for (const Node& f : natFuncs)
    std::cerr << "  natFunc: " << f << " :: " << f.getType() << "\n";

  Node zero = d_nm->mkConstInt(Rational(0));
  std::vector<Node> newAssertions;

  // ------------------------------------------------------------------
  // Phase 2a: lift free Nat$ variables to Int, add v' >= 0
  // ------------------------------------------------------------------
  for (const Node& natVar : natVars)
  {
    std::string intName = "lift_" + natVar.getName();
    Node intVar = NodeManager::mkRawSymbol(intName, d_nm->integerType());
    d_varNatToInt.insert(natVar, intVar);
    newAssertions.push_back(d_nm->mkNode(Kind::GEQ, intVar, zero));
  }

  // ------------------------------------------------------------------
  // Phase 2b: lift Nat$-related functions; total-def adds axioms
  // ------------------------------------------------------------------
  for (const Node& natFunc : natFuncs)
  {
    TypeNode origFuncType = natFunc.getType();
    size_t arity          = origFuncType.getNumChildren() - 1;
    bool retWasNat        = isNat(origFuncType[arity]);

    // Check whether the function name encodes a built-in arithmetic op.
    Kind arithKind = prefixToArithKind(natFunc.getName());
    if (arithKind != Kind::UNDEFINED_KIND)
    {
      // Map original function -> arithmetic Kind; no UF symbol needed.
      d_funcArithKind[natFunc] = arithKind;

      // Total definition: add  forall x0,...,xn. (premises) => arithOp(x0,...,xn) >= 0
      if (USE_TOTAL_DEFINITION && retWasNat && arity > 0)
      {
        TypeNode intFuncType = createIntAnalogue(origFuncType);
        std::vector<Node> bvars;
        bvars.reserve(arity);
        for (size_t i = 0; i < arity; ++i)
        {
          bvars.push_back(
              NodeManager::mkBoundVar("x" + std::to_string(i), intFuncType[i]));
        }

        std::vector<Node> premises;
        for (size_t i = 0; i < arity; ++i)
        {
          if (isNat(origFuncType[i]))
          {
            premises.push_back(d_nm->mkNode(Kind::GEQ, bvars[i], zero));
          }
        }

        Node app        = d_nm->mkNode(arithKind, bvars);
        Node conclusion = d_nm->mkNode(Kind::GEQ, app, zero);

        Node body;
        if (premises.empty())
          body = conclusion;
        else if (premises.size() == 1)
          body = d_nm->mkNode(Kind::IMPLIES, premises[0], conclusion);
        else
          body = d_nm->mkNode(
              Kind::IMPLIES, d_nm->mkNode(Kind::AND, premises), conclusion);

        Node bvList = d_nm->mkNode(Kind::BOUND_VAR_LIST, bvars);
        newAssertions.push_back(d_nm->mkNode(Kind::FORALL, bvList, body));
      }
      continue;
    }

    TypeNode intFuncType = createIntAnalogue(origFuncType);
    std::string intName  = "lift_" + natFunc.getName();

    Node intFunc = NodeManager::mkRawSymbol(intName, intFuncType);
    d_funcNatToInt.insert(natFunc, intFunc);

    // Track whether the original function returned Nat$ (used by both methods)
    if (retWasNat)
    {
      d_liftedNatRetFuncs.insert(intFunc);
    }

    // -- total definition only: add
    //      forall x1,...,xn. (premises) => f'(x1,...,xn) >= 0
    if (USE_TOTAL_DEFINITION && retWasNat)
    {
      std::vector<Node> bvars;
      bvars.reserve(arity);
      for (size_t i = 0; i < arity; ++i)
      {
        bvars.push_back(
            NodeManager::mkBoundVar("x" + std::to_string(i), intFuncType[i]));
      }

      // Premise: conjunction of (xi >= 0) for each arg originally Nat$
      std::vector<Node> premises;
      for (size_t i = 0; i < arity; ++i)
      {
        if (isNat(origFuncType[i]))
        {
          premises.push_back(d_nm->mkNode(Kind::GEQ, bvars[i], zero));
        }
      }

      // Conclusion: f'(x1,...,xn) >= 0
      Node app;
      if (arity == 0)
      {
        app = intFunc;
      }
      else
      {
        std::vector<Node> appArgs;
        appArgs.push_back(intFunc);
        appArgs.insert(appArgs.end(), bvars.begin(), bvars.end());
        app = d_nm->mkNode(Kind::APPLY_UF, appArgs);
      }
      Node conclusion = d_nm->mkNode(Kind::GEQ, app, zero);

      Node body;
      if (premises.empty())
      {
        body = conclusion;
      }
      else if (premises.size() == 1)
      {
        body = d_nm->mkNode(Kind::IMPLIES, premises[0], conclusion);
      }
      else
      {
        body = d_nm->mkNode(
            Kind::IMPLIES, d_nm->mkNode(Kind::AND, premises), conclusion);
      }

      Node axiom;
      if (arity == 0)
      {
        axiom = body;
      }
      else
      {
        Node bvList = d_nm->mkNode(Kind::BOUND_VAR_LIST, bvars);
        axiom       = d_nm->mkNode(Kind::FORALL, bvList, body);
      }
      newAssertions.push_back(axiom);
    }
  }

  // ------------------------------------------------------------------
  // Phase 3: structurally lift existing assertions; collect partial
  //          constraints for the partial method
  // ------------------------------------------------------------------
  for (size_t i = 0, n = assertionsToPreprocess->size(); i < n; ++i)
  {
    Node orig   = (*assertionsToPreprocess)[i];
    Node lifted = liftNodeInternal(orig);
    if (lifted != orig)
    {
      assertionsToPreprocess->replace(i, lifted);
    }

    // partial definition: top-level constraints from the lifted assertion
    if (!USE_TOTAL_DEFINITION)
    {
      std::vector<Node> partialConstraints;
      collectPartialConstraints(lifted, partialConstraints);
      for (const Node& c : partialConstraints)
      {
        newAssertions.push_back(c);
      }
    }
  }

  // ------------------------------------------------------------------
  // Phase 4: append auxiliary assertions (v' >= 0, axioms, partial)
  // ------------------------------------------------------------------
  for (const Node& a : newAssertions)
  {
    assertionsToPreprocess->push_back(a);
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
    {
      argTypes.push_back(createIntAnalogue(tn[i]));
    }
    TypeNode retType = createIntAnalogue(tn[arity]);
    return d_nm->mkFunctionType(argTypes, retType);
  }
  return tn;
}

// ===========================================================================
// Symbol collection
// ===========================================================================

void NatToInt::collectNatSymbols(TNode root,
                                 std::unordered_set<Node>& natVars,
                                 std::unordered_set<Node>& natFuncs,
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

    // For APPLY_UF the function symbol is an "operator" not a regular child;
    // we must inspect it explicitly.
    if (cur.getMetaKind() == metakind::PARAMETERIZED)
    {
      Node op = cur.getOperator();
      if (!visited.count(op))
      {
        visited.insert(op);
        TypeNode opType = op.getType();
        if (opType.isFunction() && hasNatInSignature(opType))
        {
          natFuncs.insert(op);
        }
      }
    }

    if (cur.getNumChildren() == 0)
    {
      // Only free variables of sort Nat$; bound variables are handled inside
      // liftQuantifier when the enclosing FORALL/EXISTS is processed.
      TypeNode tn = cur.getType();
      if (isNat(tn) && cur.getKind() != Kind::BOUND_VARIABLE
          && cur.getKind() != Kind::CONST_INTEGER)
      {
        natVars.insert(cur);
      }
    }
    else
    {
      for (TNode child : cur)
      {
        toVisit.push_back(child);
      }
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

  // FORALL / EXISTS: delegate to liftQuantifier (handles bound vars +
  // partial-def body constraints + nat-var guard)
  if (k == Kind::FORALL || k == Kind::EXISTS)
  {
    result = liftQuantifier(n);
    d_lifted.insert(n, result);
    return result;
  }

  // Leaf: look up in the variable map (covers free and bound Nat$ vars
  // registered by an enclosing liftQuantifier call)
  if (n.getNumChildren() == 0)
  {
    // Integer literal used as Nat$ value: re-emit as a proper Int constant.
    if (n.getKind() == Kind::CONST_INTEGER)
    {
      result = d_nm->mkConstInt(n.getConst<Rational>());
      d_lifted.insert(n, result);
      return result;
    }
    NodeMap::iterator vit = d_varNatToInt.find(n);
    result = (vit != d_varNatToInt.end()) ? (*vit).second : Node(n);
    d_lifted.insert(n, result);
    return result;
  }

  // Non-leaf: recurse, replacing the operator for PARAMETERIZED kinds
  bool changed = false;
  std::vector<Node> newChildren;

  if (n.getMetaKind() == metakind::PARAMETERIZED)
  {
    Node op = n.getOperator();

    // Check if this UF encodes a built-in arithmetic operation.
    auto arithIt = d_funcArithKind.find(op);
    if (arithIt != d_funcArithKind.end())
    {
      // Lift arguments and build the arithmetic node (no UF operator).
      std::vector<Node> arithArgs;
      for (TNode child : n)
      {
        arithArgs.push_back(liftNodeInternal(child));
      }
      result = d_nm->mkNode(arithIt->second, arithArgs);
      // Partial def: track nodes from Nat$-returning arithmetic functions.
      TypeNode opType = op.getType();
      if (isNat(opType[opType.getNumChildren() - 1]))
      {
        d_arithNatRetApps.insert(result);
      }
      d_lifted.insert(n, result);
      return result;
    }

    NodeMap::iterator fop = d_funcNatToInt.find(op);
    Node newOp = (fop != d_funcNatToInt.end()) ? (*fop).second : op;
    newChildren.push_back(newOp);
    changed = (newOp != op);
  }

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
// Quantifier lifting
// ===========================================================================

Node NatToInt::liftQuantifier(TNode n)
{
  Assert(n.getKind() == Kind::FORALL || n.getKind() == Kind::EXISTS);

  Kind  qkind  = n.getKind();
  TNode bvList = n[0];
  TNode body   = n[1];
  Node  zero   = d_nm->mkConstInt(Rational(0));

  // --- 1. Create Int bound-vars for Nat$ bound-vars ----------------------
  std::vector<Node> newBvars;
  std::vector<Node> natVarGuards;
  bool anyNat = false;

  for (TNode bv : bvList)
  {
    if (isNat(bv.getType()))
    {
      // Fresh Int bound variable named lift_<original>
      Node intBv = NodeManager::mkBoundVar("lift_" + bv.getName(),
                                           d_nm->integerType());
      // Register so liftNodeInternal replaces bv -> intBv in the body
      d_varNatToInt.insert(bv, intBv);
      newBvars.push_back(intBv);
      natVarGuards.push_back(d_nm->mkNode(Kind::GEQ, intBv, zero));
      anyNat = true;
    }
    else
    {
      newBvars.push_back(Node(bv));
    }
  }

  Node newBvList = d_nm->mkNode(Kind::BOUND_VAR_LIST, newBvars);

  // --- 2. Lift the body -------------------------------------------------
  Node liftedBody = liftNodeInternal(body);

  // --- 3. (Partial def) fold same-scope body constraints into the body --
  //
  //   collectPartialConstraints stops at nested FORALL/EXISTS, so only
  //   constraints for the direct body level are returned here.
  if (!USE_TOTAL_DEFINITION)
  {
    std::vector<Node> bodyConstraints;
    collectPartialConstraints(liftedBody, bodyConstraints);

    if (!bodyConstraints.empty())
    {
      // AND the function constraints in front of the body
      bodyConstraints.push_back(liftedBody);
      liftedBody = d_nm->mkNode(Kind::AND, bodyConstraints);
    }
  }

  // --- 4. Apply non-negativity guard for Nat$ bound vars ----------------
  //
  //   FORALL: (v1'>=0 /\ ... /\ vm'>=0) -> liftedBody
  //   EXISTS: (v1'>=0 /\ ... /\ vm'>=0) /\ liftedBody
  Node newBody;
  if (!anyNat)
  {
    newBody = liftedBody;
  }
  else
  {
    Node guard;
    if (natVarGuards.size() == 1)
    {
      guard = natVarGuards[0];
    }
    else
    {
      guard = d_nm->mkNode(Kind::AND, natVarGuards);
    }

    newBody = (qkind == Kind::FORALL)
                  ? d_nm->mkNode(Kind::IMPLIES, guard, liftedBody)
                  : d_nm->mkNode(Kind::AND, guard, liftedBody);
  }

  // Preserve optional third child (INST_PATTERN_LIST) if present
  if (n.getNumChildren() > 2)
  {
    Node patterns = liftNodeInternal(n[2]);
    return d_nm->mkNode(qkind, newBvList, newBody, patterns);
  }

  return d_nm->mkNode(qkind, newBvList, newBody);
}

// ===========================================================================
// Partial definition constraint collection
// ===========================================================================

void NatToInt::collectPartialConstraints(TNode liftedExpr,
                                         std::vector<Node>& out)
{
  // Stop at quantifier boundaries; constraints inside have already been
  // folded into the body by liftQuantifier.
  Kind k = liftedExpr.getKind();
  if (k == Kind::FORALL || k == Kind::EXISTS)
  {
    return;
  }

  // For APPLY_UF (and any PARAMETERIZED node): check the operator.
  // If the operator is a lifted Nat$-returning function, add expr >= 0.
  if (liftedExpr.getMetaKind() == metakind::PARAMETERIZED)
  {
    Node op = liftedExpr.getOperator();
    if (d_liftedNatRetFuncs.find(op) != d_liftedNatRetFuncs.end())
    {
      Node zero = d_nm->mkConstInt(Rational(0));
      out.push_back(d_nm->mkNode(Kind::GEQ, Node(liftedExpr), zero));
    }
  }

  // For arithmetic nodes that originated from a Nat$-returning function,
  // add expr >= 0 at the same scope.
  if (d_arithNatRetApps.find(Node(liftedExpr)) != d_arithNatRetApps.end())
  {
    Node zero = d_nm->mkConstInt(Rational(0));
    out.push_back(d_nm->mkNode(Kind::GEQ, Node(liftedExpr), zero));
  }

  // Recurse into all children (arguments for APPLY_UF, sub-formulas for
  // connectives, etc.).  This ensures that Nat$-returning applications
  // nested inside non-Nat$-returning ones are also captured.
  for (TNode child : liftedExpr)
  {
    collectPartialConstraints(child, out);
  }
}

}  // namespace passes
}  // namespace preprocessing
}  // namespace cvc5::internal
