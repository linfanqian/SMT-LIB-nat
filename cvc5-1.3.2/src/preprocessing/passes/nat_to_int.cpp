/* The natural number preprocessing pass.
 *
 * Add total definition for natural numbers:
 * ∀x. x >= 0  => f'(x) >= 0 
 */

#include "preprocessing/passes/nat_to_int.h"
#include "preprocessing/assertion_pipeline.h"
#include "preprocessing/preprocessing_pass_context.h"
#include "util/rational.h"

using namespace cvc5::internal::kind;

namespace cvc5::internal {
namespace preprocessing {
namespace passes {

NatToInt::NatToInt(PreprocessingPassContext* preprocContext)
    : PreprocessingPass(preprocContext, "nat-to-int"),
    d_nm(nodeManager())
{
}

PreprocessingPassResult NatToInt::applyInternal(
    AssertionPipeline* assertionsToPreprocess)
{
    // Collect all Nat symbols
    std::unordered_set<Node> natVars;
    std::unordered_set<Node> natFuncs;
    std::unordered_set<Node> visited;

    for (size_t i = 0, n = assertionsToPreprocess->size(); i < n; ++i) {
        collectNatSymbols((*assertionsToPreprocess)[i], natVars, natFuncs, visited);
    }

    // Create lifted integer and ">= 0" assertions
    std::vector<Node> newAssertions;
    Node zero = d_nm->mkConstInt(Rational(0));

    // variables
    for (const Node& natVar : natVars) {
        std::string intName  = natVar.getName() + "_int";

        Node intVar = NodeManager::mkRawSymbol(intName, d_nm->integerType());
        d_varNatToInt[natVar] = intVar;

        // Assert intVar >= 0.
        Node nonNeg = d_nm->mkNode(Kind::GEQ, intVar, zero);
        newAssertions.push_back(nonNeg);
    }

    // functions
    for (const Node& natFunc : natFuncs) {
        TypeNode origFuncType = natFunc.getType();
        TypeNode intFuncType  = createIntAnalogue(origFuncType);
        std::string intName  = natFunc.getName() + "_int";

        Node intFunc = NodeManager::mkRawSymbol(intName, intFuncType);
        d_funcNatToInt[natFunc] = intFunc;

        // universally-quantified constraint
        size_t arity = origFuncType.getNumChildren() - 1;  // last child is return type

        // bound variables for the quantifier.
        std::vector<Node> bvars;
        bvars.reserve(arity);
        for (size_t i = 0; i < arity; ++i) {
            TypeNode argTypeInt = intFuncType[i];  // already replaced
            bvars.push_back(d_nm->mkBoundVar("x" + std::to_string(i), argTypeInt));
        }
        Node bvList = d_nm->mkNode(Kind::BOUND_VAR_LIST, bvars);

        // premise: conjunction of (xi >= 0) for each arg that was Nat.
        std::vector<Node> premises;
        for (size_t i = 0; i < arity; ++i) {
            if (isNat(origFuncType[i])) {
                premises.push_back(d_nm->mkNode(Kind::GEQ, bvars[i], zero));
            }
        }

        // Build conclusion: intFun(bvars) >= 0   (only if return type was Nat$)
        bool retWasNat = isNat(origFuncType[arity]);

        if (retWasNat || !premises.empty()) {
            Node app;
            if (arity == 0) {
                app = intFunc;  // nullary – treat as a constant
            } else {
                std::vector<Node> appArgs;
                appArgs.push_back(intFunc);
                appArgs.insert(appArgs.end(), bvars.begin(), bvars.end());
                app = d_nm->mkNode(Kind::APPLY_UF, appArgs);
            }

            if (retWasNat) {
                Node conclusion = d_nm->mkNode(Kind::GEQ, app, zero);

                Node body;
                if (premises.empty()) {
                    body = conclusion;
                } else if (premises.size() == 1) {
                    body = d_nm->mkNode(Kind::IMPLIES, premises[0], conclusion);
                } else {
                    Node premiseConj = d_nm->mkNode(Kind::AND, premises);
                    body = d_nm->mkNode(Kind::IMPLIES, premiseConj, conclusion);
                }

                Node axiom;
                if (arity == 0) {
                    axiom = body;
                } else {
                    axiom = d_nm->mkNode(Kind::FORALL, bvList, body);
                }
                newAssertions.push_back(axiom);
            }
        }
    }

    // Rewrite existing assertions
    for (size_t i = 0, n = assertionsToPreprocess->size(); i < n; ++i) {
        Node orig    = (*assertionsToPreprocess)[i];
        Node rewrite = replaceNatToInt(orig);
        if (rewrite != orig) {
            assertionsToPreprocess->replace(i, rewrite);
        }
    }

    // Prepend newly added >= 0 assertions
    for (const Node& a : newAssertions) {
        assertionsToPreprocess->push_back(a);
    }

    return PreprocessingPassResult::NO_CONFLICT;
}

bool NatToInt::isNat(const TypeNode& tn) const {
    // Directly return if sort is not an uninterpreted sort
    if (!tn.isUninterpretedSort()) return false;

    // See whether the name of this sort is Nat$
    return tn.getName() == "Nat$";
}

void NatToInt::collectNatSymbols(TNode root, std::unordered_set<Node>& natVars,
    std::unordered_set<Node>& natFuncs, std::unordered_set<Node>& visited) {
    std::vector<Node> toVisit;
    toVisit.push_back(root);

    while (!toVisit.empty()) {
        Node cur = toVisit.back();
        toVisit.pop_back();

        if (visited.count(cur)) continue;
        visited.insert(cur);

        if (cur.isVar() || cur.isConst()) {
            if (isNat(cur.getType())) {
                natVars.insert(cur);
            }
        } else {
            for (const TNode& child : cur) {
                toVisit.push_back(child);
            }
        }
    }
}

TypeNode NatToInt::createIntAnalogue(const TypeNode& tn) {
    if (isNat(tn)) return d_nm->integerType();

    if (tn.isFunction())
    {
        // Collect argument types and return type.
        std::vector<TypeNode> argTypes;
        size_t argNum = tn.getNumChildren() - 1;   // last one is return type
        for (size_t i = 0; i < argNum; ++i) {
        argTypes.push_back(createIntAnalogue(tn[i]));
        }
        TypeNode retType = createIntAnalogue(tn[argNum]);
        return d_nm->mkFunctionType(argTypes, retType);
    }

    return tn;
}

Node NatToInt::replaceNatToInt(TNode n) {
    // Check whether the node has been lifted
    auto it = d_lifted.find(n);
    if (it != d_lifted.end()) return it->second;

    Node result;

    // Node is a cached Nat variable
    auto vit = d_varNatToInt.find(n);
    if (vit != d_varNatToInt.end())
    {
        result = vit->second;
        d_lifted[n] = result;
        return result;
    }

    // Node is a cached Nat function
    auto fit = d_funcNatToInt.find(n);
    if (fit != d_funcNatToInt.end())
    {
        result = fit->second;
        d_lifted[n] = result;
        return result;
    }

    // Node is not lifted, not a Nat, and it's a leaf node
    if (n.getNumChildren() == 0) {
        result = Node(n);
        d_lifted[n] = result;
        return result;
    }

    // Recursion on children
    std::vector<Node> newChildren;
    newChildren.reserve(n.getNumChildren());
    bool changed = false;

    for (const TNode& child : n) {
        Node newChild = replaceNatToInt(child);
        newChildren.push_back(newChild);
        changed = (newChild != child) || changed;
    }

    if (changed){
        result = d_nm->mkNode(n.getKind(), newChildren);
    } else {
        result = Node(n);
    }

    d_lifted[n] = result;
    return result;
}

}  // namespace passes
}  // namespace preprocessing
}  // namespace cvc5::internal
