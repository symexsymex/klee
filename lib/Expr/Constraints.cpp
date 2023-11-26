//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr/Constraints.h"

#include "klee/Expr/ArrayExprVisitor.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprHashMap.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Expr/Path.h"
#include "klee/Expr/Symcrete.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Support/OptionCategories.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"
DISABLE_WARNING_POP

#include <map>

using namespace klee;

namespace {
llvm::cl::opt<RewriteEqualitiesPolicy> RewriteEqualities(
    "rewrite-equalities",
    llvm::cl::desc("Rewrite existing constraints when an equality with a "
                   "constant is added (default=simple)"),
    llvm::cl::values(clEnumValN(RewriteEqualitiesPolicy::None, "none",
                                "Don't rewrite"),
                     clEnumValN(RewriteEqualitiesPolicy::Simple, "simple",
                                "lightweight visitor"),
                     clEnumValN(RewriteEqualitiesPolicy::Full, "full",
                                "more powerful visitor")),
    llvm::cl::init(RewriteEqualitiesPolicy::Simple), llvm::cl::cat(SolvingCat));
} // namespace

class ExprReplaceVisitor : public ExprVisitor {
private:
  ref<Expr> src, dst;

public:
  ExprReplaceVisitor(const ref<Expr> &_src, const ref<Expr> &_dst)
      : src(_src), dst(_dst) {}

  Action visitExpr(const Expr &e) override {
    if (e == *src) {
      return Action::changeTo(dst);
    }
    return Action::doChildren();
  }

  Action visitExprPost(const Expr &e) override {
    if (e == *src) {
      return Action::changeTo(dst);
    }
    return Action::doChildren();
  }
};

class ExprReplaceVisitor2 : public ExprVisitor {
private:
  std::vector<std::reference_wrapper<const ExprHashMap<ref<Expr>>>>
      replacements;
  const ExprHashMap<ref<Expr>> &replacementParents;

public:
  explicit ExprReplaceVisitor2(const ExprHashMap<ref<Expr>> &_replacements,
                               const ExprHashMap<ref<Expr>> &_parents)
      : ExprVisitor(true), replacements({_replacements}),
        replacementParents(_parents) {}

  Action visitExpr(const Expr &e) override {
    for (auto i = replacements.rbegin(); i != replacements.rend(); i++) {
      if (i->get().count(ref<Expr>(const_cast<Expr *>(&e)))) {
        auto replacement = i->get().at(ref<Expr>(const_cast<Expr *>(&e)));
        if (replacementParents.count(const_cast<Expr *>(&e))) {
          replacementDependency.insert(
              replacementParents.at(const_cast<Expr *>(&e)));
        }
        return Action::changeTo(replacement);
      }
    }
    return Action::doChildren();
  }

  Action visitExprPost(const Expr &e) override {
    for (auto i = replacements.rbegin(); i != replacements.rend(); i++) {
      if (i->get().count(ref<Expr>(const_cast<Expr *>(&e)))) {
        auto replacement = i->get().at(ref<Expr>(const_cast<Expr *>(&e)));
        if (replacementParents.count(const_cast<Expr *>(&e))) {
          replacementDependency.insert(
              replacementParents.at(const_cast<Expr *>(&e)));
        }
        return Action::changeTo(replacement);
      }
    }
    return Action::doChildren();
  }

  Action visitSelect(const SelectExpr &sexpr) override {
    auto cond = visit(sexpr.cond);
    if (auto CE = dyn_cast<ConstantExpr>(cond)) {
      return CE->isTrue() ? Action::changeTo(visit(sexpr.trueExpr))
                          : Action::changeTo(visit(sexpr.falseExpr));
    }

    auto trueExpr = visit(sexpr.trueExpr);

    auto falseExpr = visit(sexpr.falseExpr);

    if (trueExpr != sexpr.trueExpr || falseExpr != sexpr.falseExpr) {
      ref<Expr> seres = SelectExpr::create(cond, trueExpr, falseExpr);

      auto res = visitExprPost(*seres.get());
      if (res.kind == Action::ChangeTo) {
        seres = res.argument;
      }
      return Action::changeTo(seres);
    } else {
      return Action::skipChildren();
    }
  }

public:
  ExprHashSet replacementDependency;
};

class ExprReplaceVisitor3 : public ExprVisitor {
private:
  std::vector<std::reference_wrapper<const ExprHashMap<ref<Expr>>>>
      replacements;
  const ExprHashMap<ref<Expr>> &replacementParents;

public:
  explicit ExprReplaceVisitor3(const ExprHashMap<ref<Expr>> &_replacements,
                               const ExprHashMap<ref<Expr>> &_parents)
      : ExprVisitor(true), replacements({_replacements}),
        replacementParents(_parents) {}

  Action visitExpr(const Expr &e) override {
    for (auto i = replacements.rbegin(); i != replacements.rend(); i++) {
      if (i->get().count(ref<Expr>(const_cast<Expr *>(&e)))) {
        auto replacement = i->get().at(ref<Expr>(const_cast<Expr *>(&e)));
        if (replacementParents.count(const_cast<Expr *>(&e))) {
          replacementDependency.insert(
              replacementParents.at(const_cast<Expr *>(&e)));
        }
        return Action::changeTo(replacement);
      }
    }
    return Action::doChildren();
  }

  Action visitExprPost(const Expr &e) override {
    for (auto i = replacements.rbegin(); i != replacements.rend(); i++) {
      if (i->get().count(ref<Expr>(const_cast<Expr *>(&e)))) {
        auto replacement = i->get().at(ref<Expr>(const_cast<Expr *>(&e)));
        if (replacementParents.count(const_cast<Expr *>(&e))) {
          replacementDependency.insert(
              replacementParents.at(const_cast<Expr *>(&e)));
        }
        return Action::changeTo(replacement);
      }
    }
    return Action::doChildren();
  }

  ExprHashSet replacementDependency;
};

ConstraintSet::ConstraintSet(constraints_ty cs, symcretes_ty symcretes,
                             Assignment concretization)
    : _constraints(cs), _symcretes(symcretes), _concretization(concretization) {
}

ConstraintSet::ConstraintSet() : _concretization(Assignment(true)) {}

void ConstraintSet::addConstraint(ref<Expr> e, const Assignment &delta) {
  _constraints.insert(e);

  // Update bindings
  for (auto i : delta.bindings) {
    _concretization.bindings[i.first] = i.second;
  }
}

IDType Symcrete::idCounter = 0;

void ConstraintSet::addSymcrete(ref<Symcrete> s,
                                const Assignment &concretization) {
  _symcretes.insert(s);
  for (auto i : s->dependentArrays()) {
    _concretization.bindings[i] = concretization.bindings.at(i);
  }
}

bool ConstraintSet::isSymcretized(ref<Expr> expr) const {
  for (auto symcrete : _symcretes) {
    if (symcrete->symcretized == expr) {
      return true;
    }
  }
  return false;
}

void ConstraintSet::rewriteConcretization(const Assignment &a) {
  for (auto i : a.bindings) {
    if (concretization().bindings.count(i.first)) {
      _concretization.bindings[i.first] = i.second;
    }
  }
}

void ConstraintSet::print(llvm::raw_ostream &os) const {
  os << "Constraints [\n";
  for (const auto &constraint : _constraints) {
    constraint->print(os);
    os << "\n";
  }

  os << "]\n";
  os << "Symcretes [\n";
  for (const auto &symcrete : _symcretes) {
    symcrete->symcretized->print(os);
    os << "\n";
  }
  os << "]\n";
}

void ConstraintSet::dump() const { this->print(llvm::errs()); }

void ConstraintSet::changeCS(constraints_ty &cs) { _constraints = cs; }

const constraints_ty &ConstraintSet::cs() const { return _constraints; }

const symcretes_ty &ConstraintSet::symcretes() const { return _symcretes; }

const Path &PathConstraints::path() const { return _path; }

const ExprHashMap<Path::PathIndex> &PathConstraints::indexes() const {
  return pathIndexes;
}

const Assignment &ConstraintSet::concretization() const {
  return _concretization;
}

const constraints_ty &PathConstraints::original() const { return _original; }

const ExprHashMap<ExprHashSet> &PathConstraints::simplificationMap() const {
  return _simplificationMap;
}

const ConstraintSet &PathConstraints::cs() const { return constraints; }

ConstraintSet PathConstraints::withAssumtions(const ExprHashSet &assumptions) const {
  auto result = constraints;
  for (auto assump : assumptions) {
    result.addConstraint(assump, {});
  }
  return result;
}

const PathConstraints::path_ordered_constraints_ty &
PathConstraints::orderedCS() const {
  return orderedConstraints;
}

void PathConstraints::advancePath(KInstruction *prevPC, KInstruction *pc) {
  _path.stepInstruction(prevPC, pc);
}

void PathConstraints::retractPath() {
  _path.retractInstruction();
}

void PathConstraints::advancePath(const Path &path) {
  _path = Path::concat(_path, path);
}

ExprHashSet PathConstraints::addConstraint(ref<Expr> e, const Assignment &delta,
                                           Path::PathIndex currIndex) {
  auto expr = Simplificator::simplifyExpr(constraints, e);
  if (auto ce = dyn_cast<ConstantExpr>(expr.simplified)) {
    assert(ce->isTrue() && "Attempt to add invalid constraint");
    return {};
  }
  ExprHashSet added;
  std::vector<ref<Expr>> exprs;
  Expr::splitAnds(expr.simplified, exprs);
  for (auto expr : exprs) {
    if (auto ce = dyn_cast<ConstantExpr>(expr)) {
      assert(ce->isTrue() && "Expression simplified to false");
    } else {
      _original.insert(expr);
      added.insert(expr);
      pathIndexes.insert({expr, currIndex});
      _simplificationMap[expr].insert(expr);
      orderedConstraints[currIndex].push_back(expr);
      constraints.addConstraint(expr, delta);
    }
  }

  if (RewriteEqualities != RewriteEqualitiesPolicy::None) {
    auto simplified =
        Simplificator::simplify(constraints.cs(), RewriteEqualities);
    constraints.changeCS(simplified.simplified);

    _simplificationMap = Simplificator::composeExprDependencies(
        _simplificationMap, simplified.dependency);
  }

  return added;
}

ExprHashSet PathConstraints::addConstraint(ref<Expr> e,
                                           const Assignment &delta) {
  return addConstraint(e, delta, _path.getCurrentIndex());
}

bool PathConstraints::isSymcretized(ref<Expr> expr) const {
  return constraints.isSymcretized(expr);
}

void PathConstraints::addSymcrete(ref<Symcrete> s,
                                  const Assignment &concretization) {
  constraints.addSymcrete(s, concretization);
}

void PathConstraints::rewriteConcretization(const Assignment &a) {
  constraints.rewriteConcretization(a);
}

Simplificator::ExprResult
Simplificator::simplifyExpr(const constraints_ty &constraints,
                            const ref<Expr> &expr) {
  if (isa<ConstantExpr>(expr))
    return {expr, {}};

  ExprHashMap<ref<Expr>> equalities;
  ExprHashMap<ref<Expr>> equalitiesParents;

  for (auto &constraint : constraints) {
    if (const EqExpr *ee = dyn_cast<EqExpr>(constraint)) {
      ref<Expr> left = ee->left;
      ref<Expr> right = ee->right;
      if (right->height() < left->height()) {
        left = ee->right;
        right = ee->left;
      }
      if (isa<ConstantExpr>(ee->left)) {
        equalities.insert(std::make_pair(ee->right, ee->left));
        equalitiesParents.insert({ee->right, constraint});
      } else {
        equalities.insert(std::make_pair(constraint, Expr::createTrue()));
        equalities.insert(std::make_pair(right, left));
        equalitiesParents.insert({constraint, constraint});
        equalitiesParents.insert({right, constraint});
      }
    } else {
      equalities.insert(std::make_pair(constraint, Expr::createTrue()));
      equalitiesParents.insert({constraint, constraint});
      if (const NotExpr *ne = dyn_cast<NotExpr>(constraint)) {
        equalities.insert(std::make_pair(ne->expr, Expr::createFalse()));
        equalitiesParents.insert({ne->expr, constraint});
      }
    }
  }

  ExprReplaceVisitor2 visitor(equalities, equalitiesParents);
  auto visited = visitor.visit(expr);
  return {visited, visitor.replacementDependency};
}

Simplificator::ExprResult
Simplificator::simplifyExpr(const ConstraintSet &constraints,
                            const ref<Expr> &expr) {
  return simplifyExpr(constraints.cs(), expr);
}

Simplificator::SetResult
Simplificator::simplify(const constraints_ty &constraints,
                        RewriteEqualitiesPolicy policy) {
  // Initialization
  constraints_ty simplified;
  ExprHashMap<ExprHashSet> dependencies;
  for (auto constraint : constraints) {
    simplified.insert(constraint);
    dependencies.insert({constraint, {constraint}});
  }

  bool changed = true;
  while (changed) {
    changed = false;
    Replacements replacements = gatherReplacements(simplified);
    constraints_ty currentSimplified;
    ExprHashMap<ExprHashSet> currentDependencies;

    for (auto &constraint : simplified) {
      removeReplacement(replacements, constraint);
      ref<Expr> simplifiedConstraint;
      ExprHashSet dependency;
      if (policy == RewriteEqualitiesPolicy::Simple) {
        auto visitor = ExprReplaceVisitor3(replacements.equalities,
                                           replacements.equalitiesParents);
        simplifiedConstraint = visitor.visit(constraint);
        dependency = visitor.replacementDependency;
      } else {
        assert(policy != RewriteEqualitiesPolicy::None);
        auto visitor = ExprReplaceVisitor2(replacements.equalities,
                                           replacements.equalitiesParents);
        simplifiedConstraint = visitor.visit(constraint);
        dependency = visitor.replacementDependency;
      }
      addReplacement(replacements, constraint);
      std::vector<ref<Expr>> andsSplit;
      Expr::splitAnds(simplifiedConstraint, andsSplit);
      for (auto part : andsSplit) {
        currentSimplified.insert(part);
        currentDependencies.insert({part, dependency});
        currentDependencies[part].insert(constraint);
      }
      if (constraint != simplifiedConstraint || andsSplit.size() > 1) {
        changed = true;
      }
    }

    if (changed) {
      simplified = currentSimplified;
      dependencies = composeExprDependencies(dependencies, currentDependencies);
    }
  }

  simplified.erase(ConstantExpr::createTrue());
  dependencies.erase(ConstantExpr::createTrue());

  return {simplified, dependencies};
}

Simplificator::Replacements
Simplificator::gatherReplacements(constraints_ty constraints) {
  Replacements result;
  for (auto &constraint : constraints) {
    if (const EqExpr *ee = dyn_cast<EqExpr>(constraint)) {
      if (isa<ConstantExpr>(ee->left)) {
        result.equalities.insert(std::make_pair(ee->right, ee->left));
        result.equalitiesParents.insert({ee->right, constraint});
      } else {
        result.equalities.insert(
            std::make_pair(constraint, Expr::createTrue()));
        result.equalitiesParents.insert({constraint, constraint});
      }
    } else {
      result.equalities.insert(std::make_pair(constraint, Expr::createTrue()));
      result.equalitiesParents.insert({constraint, constraint});
    }
  }
  return result;
}

void Simplificator::addReplacement(Replacements &replacements, ref<Expr> expr) {
  if (const EqExpr *ee = dyn_cast<EqExpr>(expr)) {
    if (isa<ConstantExpr>(ee->left)) {
      replacements.equalities.insert(std::make_pair(ee->right, ee->left));
      replacements.equalitiesParents.insert({ee->right, expr});
    } else {
      replacements.equalities.insert(std::make_pair(expr, Expr::createTrue()));
      replacements.equalitiesParents.insert({expr, expr});
    }
  } else {
    replacements.equalities.insert(std::make_pair(expr, Expr::createTrue()));
    replacements.equalitiesParents.insert({expr, expr});
  }
}

void Simplificator::removeReplacement(Replacements &replacements,
                                      ref<Expr> expr) {
  if (const EqExpr *ee = dyn_cast<EqExpr>(expr)) {
    if (isa<ConstantExpr>(ee->left)) {
      replacements.equalities.erase(ee->right);
      replacements.equalitiesParents.erase(ee->right);
    } else {
      replacements.equalities.erase(expr);
      replacements.equalitiesParents.erase(expr);
    }
  } else {
    replacements.equalities.erase(expr);
    replacements.equalitiesParents.erase(expr);
  }
}

ExprHashMap<ExprHashSet>
Simplificator::composeExprDependencies(const ExprHashMap<ExprHashSet> &upper,
                                       const ExprHashMap<ExprHashSet> &lower) {
  ExprHashMap<ExprHashSet> result;
  for (const auto &dependent : lower) {
    for (const auto &dependency : dependent.second) {
      for (const auto &upperDependency : upper.at(dependency)) {
        result[dependent.first].insert(upperDependency);
      }
    }
  }
  return result;
}

std::vector<const Array *> ConstraintSet::gatherArrays() const {
  std::vector<const Array *> arrays;
  findObjects(_constraints.begin(), _constraints.end(), arrays);
  return arrays;
}

std::vector<const Array *> ConstraintSet::gatherSymcretizedArrays() const {
  std::unordered_set<const Array *> arrays;
  for (const ref<Symcrete> &symcrete : _symcretes) {
    arrays.insert(symcrete->dependentArrays().begin(),
                  symcrete->dependentArrays().end());
  }
  return std::vector<const Array *>(arrays.begin(), arrays.end());
}
