//===-- IndependentSolver.cpp ---------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr/SymbolicSource.h"
#include "klee/Solver/SolverUtil.h"

#define DEBUG_TYPE "independent-solver"
#include "klee/Solver/Solver.h"

#include "klee/Expr/Assignment.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprHashMap.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/IndependentSet.h"
#include "klee/Solver/SolverImpl.h"
#include "klee/Support/Debug.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
DISABLE_WARNING_POP

#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

using namespace klee;
using namespace llvm;

class IndependentSolver : public SolverImpl {
private:
  std::unique_ptr<Solver> solver;

public:
  IndependentSolver(std::unique_ptr<Solver> solver)
      : solver(std::move(solver)) {}

  bool computeTruth(const Query &, bool &isValid);
  bool computeValidity(const Query &, PartialValidity &result);
  bool computeValue(const Query &, ref<Expr> &result);
  bool computeInitialValues(const Query &query,
                            const std::vector<const Array *> &objects,
                            std::vector<SparseStorage<unsigned char>> &values,
                            bool &hasSolution);
  bool check(const Query &query, ref<SolverResponse> &result);
  bool computeValidityCore(const Query &query, ValidityCore &validityCore,
                           bool &isValid);
  SolverRunStatus getOperationStatusCode();
  char *getConstraintLog(const Query &);
  void setCoreSolverTimeout(time::Span timeout);
};

bool IndependentSolver::computeValidity(const Query &query,
                                        PartialValidity &result) {
  constraints_ty required;
  IndependentElementSet eltsClosure =
      getIndependentConstraints(query, required);
  ConstraintSet tmp(
      required, eltsClosure.symcretes,
      query.constraints.concretization().part(eltsClosure.symcretes));
  return solver->impl->computeValidity(query.withConstraints(tmp), result);
}

bool IndependentSolver::computeTruth(const Query &query, bool &isValid) {
  constraints_ty required;
  IndependentElementSet eltsClosure =
      getIndependentConstraints(query, required);
  ConstraintSet tmp(
      required, eltsClosure.symcretes,
      query.constraints.concretization().part(eltsClosure.symcretes));
  return solver->impl->computeTruth(query.withConstraints(tmp), isValid);
}

bool IndependentSolver::computeValue(const Query &query, ref<Expr> &result) {
  constraints_ty required;
  IndependentElementSet eltsClosure =
      getIndependentConstraints(query, required);
  ConstraintSet tmp(
      required, eltsClosure.symcretes,
      query.constraints.concretization().part(eltsClosure.symcretes));
  return solver->impl->computeValue(query.withConstraints(tmp), result);
}

// Helper function used only for assertions to make sure point created
// during computeInitialValues is in fact correct. The ``retMap`` is used
// in the case ``objects`` doesn't contain all the assignments needed.
bool assertCreatedPointEvaluatesToTrue(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<SparseStorage<unsigned char>> &values,
    std::map<const Array *, SparseStorage<unsigned char>> &retMap) {
  // _allowFreeValues is set to true so that if there are missing bytes in the
  // assignment we will end up with a non ConstantExpr after evaluating the
  // assignment and fail
  Assignment assign = Assignment(objects, values, /*_allowFreeValues=*/true);

  // Add any additional bindings.
  // The semantics of std::map should be to not insert a (key, value)
  // pair if it already exists so we should continue to use the assignment
  // from ``objects`` and ``values``.
  if (retMap.size() > 0)
    assign.bindings.insert(retMap.begin(), retMap.end());

  for (auto const &constraint : query.constraints.cs()) {
    ref<Expr> ret = assign.evaluate(constraint);

    assert(isa<ConstantExpr>(ret) &&
           "assignment evaluation did not result in constant");
    ref<ConstantExpr> evaluatedConstraint = dyn_cast<ConstantExpr>(ret);
    if (evaluatedConstraint->isFalse()) {
      return false;
    }
  }
  ref<Expr> neg = Expr::createIsZero(query.expr);
  ref<Expr> q = assign.evaluate(neg);
  assert(isa<ConstantExpr>(q) &&
         "assignment evaluation did not result in constant");
  return cast<ConstantExpr>(q)->isTrue();
}

bool assertCreatedPointEvaluatesToTrue(
    const Query &query,
    std::map<const Array *, SparseStorage<unsigned char>> &bindings,
    std::map<const Array *, SparseStorage<unsigned char>> &retMap) {
  std::vector<const Array *> objects;
  std::vector<SparseStorage<unsigned char>> values;
  objects.reserve(bindings.size());
  values.reserve(bindings.size());
  for (auto &ovp : bindings) {
    objects.push_back(ovp.first);
    values.push_back(ovp.second);
  }
  return assertCreatedPointEvaluatesToTrue(query, objects, values, retMap);
}

bool IndependentSolver::computeInitialValues(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<SparseStorage<unsigned char>> &values, bool &hasSolution) {
  // We assume the query has a solution except proven differently
  // This is important in case we don't have any constraints but
  // we need initial values for requested array objects.
  hasSolution = true;
  // FIXME: When we switch to C++11 this should be a std::unique_ptr so we don't
  // need to remember to manually call delete
  std::list<IndependentElementSet> *factors =
      getAllIndependentConstraintsSets(query);

  // Used to rearrange all of the answers into the correct order
  std::map<const Array *, SparseStorage<unsigned char>> retMap;
  for (std::list<IndependentElementSet>::iterator it = factors->begin();
       it != factors->end(); ++it) {
    std::vector<const Array *> arraysInFactor;
    calculateArrayReferences(*it, arraysInFactor);
    // Going to use this as the "fresh" expression for the Query() invocation
    // below
    assert(it->exprs.size() >= 1 && "No null/empty factors");
    if (arraysInFactor.size() == 0) {
      continue;
    }
    ConstraintSet tmp(it->exprs, it->symcretes,
                      query.constraints.concretization().part(it->symcretes));
    std::vector<SparseStorage<unsigned char>> tempValues;
    if (!solver->impl->computeInitialValues(
            Query(tmp, ConstantExpr::alloc(0, Expr::Bool)), arraysInFactor,
            tempValues, hasSolution)) {
      values.clear();
      delete factors;
      return false;
    } else if (!hasSolution) {
      values.clear();
      delete factors;
      return true;
    } else {
      assert(tempValues.size() == arraysInFactor.size() &&
             "Should be equal number arrays and answers");
      for (unsigned i = 0; i < tempValues.size(); i++) {
        if (retMap.count(arraysInFactor[i])) {
          // We already have an array with some partially correct answers,
          // so we need to place the answers to the new query into the right
          // spot while avoiding the undetermined values also in the array
          SparseStorage<unsigned char> *tempPtr = &retMap[arraysInFactor[i]];
          assert(tempPtr->size() == tempValues[i].size() &&
                 "we're talking about the same array here");
          klee::DenseSet<unsigned> *ds = &(it->elements[arraysInFactor[i]]);
          for (std::set<unsigned>::iterator it2 = ds->begin(); it2 != ds->end();
               it2++) {
            unsigned index = *it2;
            tempPtr->store(index, tempValues[i].load(index));
          }
        } else {
          // Dump all the new values into the array
          retMap[arraysInFactor[i]] = tempValues[i];
        }
      }
    }
  }

  Assignment solutionAssignment(retMap, true);
  for (std::vector<const Array *>::const_iterator it = objects.begin();
       it != objects.end(); it++) {
    const Array *arr = *it;
    if (!retMap.count(arr)) {
      // this means we have an array that is somehow related to the
      // constraint, but whose values aren't actually required to
      // satisfy the query.
      ref<ConstantExpr> arrayConstantSize =
          dyn_cast<ConstantExpr>(solutionAssignment.evaluate(arr->size));
      assert(arrayConstantSize &&
             "Array of symbolic size had not receive value for size!");
      SparseStorage<unsigned char> ret(arrayConstantSize->getZExtValue());
      values.push_back(ret);
    } else {
      values.push_back(retMap[arr]);
    }
  }
  assert(assertCreatedPointEvaluatesToTrue(query, objects, values, retMap) &&
         "should satisfy the equation");
  delete factors;
  return true;
}

bool IndependentSolver::check(const Query &query, ref<SolverResponse> &result) {
  // We assume the query has a solution except proven differently
  // This is important in case we don't have any constraints but
  // we need initial values for requested array objects.

  // FIXME: When we switch to C++11 this should be a std::unique_ptr so we don't
  // need to remember to manually call delete
  std::list<IndependentElementSet> *factors =
      getAllIndependentConstraintsSets(query);

  // Used to rearrange all of the answers into the correct order
  std::map<const Array *, SparseStorage<unsigned char>> retMap;
  for (std::list<IndependentElementSet>::iterator it = factors->begin();
       it != factors->end(); ++it) {
    std::vector<const Array *> arraysInFactor;
    calculateArrayReferences(*it, arraysInFactor);
    // Going to use this as the "fresh" expression for the Query() invocation
    // below
    assert(it->exprs.size() >= 1 && "No null/empty factors");
    if (arraysInFactor.size() == 0) {
      continue;
    }

    constraints_ty factorConstraints = it->exprs;
    ref<Expr> factorExpr = ConstantExpr::alloc(0, Expr::Bool);
    auto factorConstraintsExprIterator =
        std::find(factorConstraints.begin(), factorConstraints.end(),
                  query.negateExpr().expr);
    if (factorConstraintsExprIterator != factorConstraints.end()) {
      factorConstraints.erase(factorConstraintsExprIterator);
      factorExpr = query.expr;
    }

    ref<SolverResponse> tempResult;
    std::vector<SparseStorage<unsigned char>> tempValues;
    if (!solver->impl->check(
            Query(ConstraintSet(
                      factorConstraints, it->symcretes,
                      query.constraints.concretization().part(it->symcretes)),
                  factorExpr),
            tempResult)) {
      delete factors;
      return false;
    } else if (isa<ValidResponse>(tempResult)) {
      delete factors;
      result = tempResult;
      return true;
    } else {
      bool success =
          tempResult->tryGetInitialValuesFor(arraysInFactor, tempValues);
      assert(success && "Can not get initial values (Independent solver)!");
      assert(tempValues.size() == arraysInFactor.size() &&
             "Should be equal number arrays and answers");
      for (unsigned i = 0; i < tempValues.size(); i++) {
        if (retMap.count(arraysInFactor[i])) {
          // We already have an array with some partially correct answers,
          // so we need to place the answers to the new query into the right
          // spot while avoiding the undetermined values also in the array
          SparseStorage<unsigned char> *tempPtr = &retMap[arraysInFactor[i]];
          assert(tempPtr->size() == tempValues[i].size() &&
                 "we're talking about the same array here");
          klee::DenseSet<unsigned> *ds = &(it->elements[arraysInFactor[i]]);
          for (std::set<unsigned>::iterator it2 = ds->begin(); it2 != ds->end();
               it2++) {
            unsigned index = *it2;
            tempPtr->store(index, tempValues[i].load(index));
          }
        } else {
          // Dump all the new values into the array
          retMap[arraysInFactor[i]] = tempValues[i];
        }
      }
    }
  }
  result = new InvalidResponse(retMap);
  std::map<const Array *, SparseStorage<unsigned char>> bindings;
  bool success = result->tryGetInitialValues(bindings);
  assert(success);
  assert(assertCreatedPointEvaluatesToTrue(query, bindings, retMap) &&
         "should satisfy the equation");
  delete factors;
  return true;
}

bool IndependentSolver::computeValidityCore(const Query &query,
                                            ValidityCore &validityCore,
                                            bool &isValid) {
  constraints_ty required;
  IndependentElementSet eltsClosure =
      getIndependentConstraints(query, required);
  ConstraintSet tmp(
      required, eltsClosure.symcretes,
      query.constraints.concretization().part(eltsClosure.symcretes));
  return solver->impl->computeValidityCore(query.withConstraints(tmp),
                                           validityCore, isValid);
}

SolverImpl::SolverRunStatus IndependentSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();
}

char *IndependentSolver::getConstraintLog(const Query &query) {
  return solver->impl->getConstraintLog(query);
}

void IndependentSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}

std::unique_ptr<Solver>
klee::createIndependentSolver(std::unique_ptr<Solver> s) {
  return std::make_unique<Solver>(
      std::make_unique<IndependentSolver>(std::move(s)));
}
