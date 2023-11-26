#ifndef KLEE_SOLVERUTIL_H
#define KLEE_SOLVERUTIL_H

#include "klee/ADT/SparseStorage.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprHashMap.h"
#include "klee/System/Time.h"

namespace klee {

enum class PartialValidity {
  /// The query is provably true.
  MustBeTrue = 1,

  /// The query is provably false.
  MustBeFalse = -1,

  /// The query is not provably false (a true assignment is known to
  /// exist).
  MayBeTrue = 2,

  /// The query is not provably true (a false assignment is known to
  /// exist).
  MayBeFalse = -2,

  /// The query is known to have both true and false assignments.
  TrueOrFalse = 0,

  /// The validity of the query is unknown.
  None = 3
};

const char *pv_to_str(PartialValidity v);

using PValidity = PartialValidity;

// Backward compatiblilty
enum class Validity { True = 1, False = -1, Unknown = 0 };

struct SolverQueryMetaData {
  /// @brief Costs for all queries issued for this state
  time::Span queryCost;
};

struct Query {
public:
  const ConstraintSet constraints;
  ref<Expr> expr;

  Query(const ConstraintSet &_constraints, ref<Expr> _expr)
      : constraints(_constraints), expr(_expr) {}

  Query(const Query &query)
      : constraints(query.constraints), expr(query.expr) {}

  /// withExpr - Return a copy of the query with the given expression.
  Query withExpr(ref<Expr> _expr) const { return Query(constraints, _expr); }

  /// withFalse - Return a copy of the query with a false expression.
  Query withFalse() const {
    return Query(constraints, ConstantExpr::alloc(0, Expr::Bool));
  }

  /// negateExpr - Return a copy of the query with the expression negated.
  Query negateExpr() const { return withExpr(Expr::createIsZero(expr)); }

  Query withConstraints(const ConstraintSet &_constraints) const {
    return Query(_constraints, expr);
  }
  /// Get all arrays that figure in the query
  std::vector<const Array *> gatherArrays() const;

  bool containsSymcretes() const;

  bool containsSizeSymcretes() const;

  friend bool operator<(const Query &lhs, const Query &rhs) {
    return lhs.constraints < rhs.constraints ||
           (lhs.constraints == rhs.constraints && lhs.expr < rhs.expr);
  }

  /// Dump query
  void dump() const;
};

struct ValidityCore {
public:
  typedef ExprOrderedSet constraints_typ;
  constraints_typ constraints;
  ref<Expr> expr;

  ValidityCore()
      : constraints(constraints_typ()),
        expr(ConstantExpr::alloc(1, Expr::Bool)) {}

  ValidityCore(const constraints_typ &_constraints, ref<Expr> _expr)
      : constraints(_constraints), expr(_expr) {}

  ValidityCore(const ExprHashSet &_constraints, ref<Expr> _expr) : expr(_expr) {
    for (auto e : _constraints) {
      constraints.insert(e);
    }
  }

  /// withExpr - Return a copy of the validity core with the given expression.
  ValidityCore withExpr(ref<Expr> _expr) const {
    return ValidityCore(constraints, _expr);
  }

  /// withFalse - Return a copy of the validity core with a false expression.
  ValidityCore withFalse() const {
    return ValidityCore(constraints, ConstantExpr::alloc(0, Expr::Bool));
  }

  /// negateExpr - Return a copy of the validity core with the expression
  /// negated.
  ValidityCore negateExpr() const { return withExpr(Expr::createIsZero(expr)); }

  /// Dump validity core
  void dump() const;

  bool equals(const ValidityCore &b) const {
    return constraints == b.constraints && expr == b.expr;
  }

  bool operator==(const ValidityCore &b) const { return equals(b); }

  bool operator!=(const ValidityCore &b) const { return !equals(b); }
};

class SolverResponse {
public:
  enum ResponseKind { Valid = 1, Invalid = -1, Unknown = 0 };

  /// @brief Required by klee::ref-managed objects
  class ReferenceCounter _refCount;

  virtual ~SolverResponse() = default;

  virtual ResponseKind getResponseKind() const = 0;

  virtual bool tryGetInitialValuesFor(
      const std::vector<const Array *> &objects,
      std::vector<SparseStorage<unsigned char>> &values) const {
    return false;
  }

  virtual bool tryGetInitialValues(Assignment::bindings_ty &values) const {
    return false;
  }

  virtual bool tryGetValidityCore(ValidityCore &validityCore) { return false; }

  static bool classof(const SolverResponse *) { return true; }

  virtual bool equals(const SolverResponse &b) const = 0;

  virtual bool lessThen(const SolverResponse &b) const = 0;

  bool operator==(const SolverResponse &b) const { return equals(b); }

  bool operator!=(const SolverResponse &b) const { return !equals(b); }

  bool operator<(const SolverResponse &b) const { return lessThen(b); }

  virtual void dump() = 0;
};

class ValidResponse : public SolverResponse {
private:
  ValidityCore result;

public:
  ValidResponse(const ValidityCore &validityCore) : result(validityCore) {}

  bool tryGetValidityCore(ValidityCore &validityCore) {
    validityCore = result;
    return true;
  }

  ValidityCore validityCore() const { return result; }

  ResponseKind getResponseKind() const { return Valid; };

  static bool classof(const SolverResponse *result) {
    return result->getResponseKind() == ResponseKind::Valid;
  }
  static bool classof(const ValidResponse *) { return true; }

  bool equals(const SolverResponse &b) const {
    if (b.getResponseKind() != ResponseKind::Valid)
      return false;
    const ValidResponse &vb = static_cast<const ValidResponse &>(b);
    return result == vb.result;
  }

  bool lessThen(const SolverResponse &b) const {
    if (b.getResponseKind() != ResponseKind::Valid)
      return true;
    const ValidResponse &vb = static_cast<const ValidResponse &>(b);
    return std::set<ref<Expr>>(result.constraints.begin(),
                               result.constraints.end()) <
           std::set<ref<Expr>>(vb.result.constraints.begin(),
                               vb.result.constraints.end());
  }

  void dump() { result.dump(); }
};

class InvalidResponse : public SolverResponse {
private:
  Assignment result;

public:
  InvalidResponse(const std::vector<const Array *> &objects,
                  std::vector<SparseStorage<unsigned char>> &values)
      : result(Assignment(objects, values)) {}

  InvalidResponse(Assignment::bindings_ty &initialValues)
      : result(initialValues) {}

  bool tryGetInitialValuesFor(
      const std::vector<const Array *> &objects,
      std::vector<SparseStorage<unsigned char>> &values) const {
    values.reserve(objects.size());
    for (auto object : objects) {
      if (result.bindings.count(object)) {
        values.push_back(result.bindings.at(object));
      } else {
        ref<ConstantExpr> constantSize =
            dyn_cast<ConstantExpr>(result.evaluate(object->size));
        assert(constantSize);
        values.push_back(
            SparseStorage<unsigned char>(constantSize->getZExtValue(), 0));
      }
    }
    return true;
  }

  bool tryGetInitialValues(Assignment::bindings_ty &values) const {
    values.insert(result.bindings.begin(), result.bindings.end());
    return true;
  }

  Assignment initialValuesFor(const std::vector<const Array *> objects) const {
    std::vector<SparseStorage<unsigned char>> values;
    tryGetInitialValuesFor(objects, values);
    return Assignment(objects, values, true);
  }

  Assignment initialValues() const {
    Assignment::bindings_ty values;
    tryGetInitialValues(values);
    return Assignment(values, true);
  }

  ResponseKind getResponseKind() const { return Invalid; };

  static bool classof(const SolverResponse *result) {
    return result->getResponseKind() == ResponseKind::Invalid;
  }
  static bool classof(const InvalidResponse *) { return true; }

  bool equals(const SolverResponse &b) const {
    if (b.getResponseKind() != ResponseKind::Invalid)
      return false;
    const InvalidResponse &ib = static_cast<const InvalidResponse &>(b);
    return result.bindings == ib.result.bindings;
  }

  bool lessThen(const SolverResponse &b) const {
    if (b.getResponseKind() != ResponseKind::Invalid)
      return false;
    const InvalidResponse &ib = static_cast<const InvalidResponse &>(b);
    return result.bindings < ib.result.bindings;
  }

  bool satisfies(std::set<ref<Expr>> &key) {
    return result.satisfies(key.begin(), key.end());
  }

  ref<Expr> evaluate(ref<Expr> &e) { return result.evaluate(e); }

  void dump() { result.dump(); }

  ref<Expr> evaluate(ref<Expr> e) { return (result.evaluate(e)); }
};

class UnknownResponse : public SolverResponse {
public:
  ResponseKind getResponseKind() const { return Unknown; };

  static bool classof(const SolverResponse *result) {
    return result->getResponseKind() == ResponseKind::Unknown;
  }
  static bool classof(const UnknownResponse *) { return true; }

  bool equals(const SolverResponse &b) const {
    return b.getResponseKind() == ResponseKind::Unknown;
  }

  bool lessThen(const SolverResponse &b) const { return false; }

  void dump() { llvm::errs() << "Unknown response"; }
};

// Fails if partial does not correspond to correct validity
Validity fromPartial(PartialValidity v);

PartialValidity toPartial(Validity v);

PartialValidity negatePartialValidity(PartialValidity pv);

}; // namespace klee
#endif
